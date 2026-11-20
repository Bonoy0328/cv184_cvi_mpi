#include "access.h"
#include "biometric_worker.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

static int trace_stall,async_enabled,async_quiescing;
static int palm_quality_status=STATUS_FAILED,face_quality_status=-1;
static float face_quality_sharpness=-2;
static uint64_t palm_quality_ms,face_quality_ms;
static int observed_hand=-1,observed_face=-1;
static uint64_t observed_hand_ms,observed_face_ms,maintenance_ms;
static uint64_t access_us(void)
{
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}
static void trace_stage(const char *kind,const char *stage,uint64_t begin,int result)
{
    if(trace_stall)printf("AUTH_STAGE modality=%s stage=%s uptime_ms=%llu elapsed_us=%llu result=%d\n",
        kind,stage,(unsigned long long)(begin/1000),(unsigned long long)(access_us()-begin),result);
}

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static AccessSnapshot shared,work;
static AccessCommand queue[8];static unsigned head,tail,count;
static int auth_enabled;
static unsigned auth_epoch,observed_auth_epoch;
static UserStore store;
static const PalmBackend *palm;
static const FaceBackend *face;
static const FingerBackend *finger;
static int observed_finger,finger_missing,finger_held,finger_latched,finger_candidates;
static uint16_t finger_candidate;
static uint64_t finger_observed_ms,finger_observed_id,finger_submitted_id,finger_unmatched;
static uint8_t finger_press_features[FINGER_ENROLL_PER_PRESS][APP_FEATURE_BYTES];
static uint32_t finger_press_sizes[FINGER_ENROLL_PER_PRESS];
static int finger_presses_done,finger_press_count;
static uint64_t finger_empty_since,finger_empty_last;
#define FINGER_LEAVE_FRAMES 5
static int finger_enrolling(void){return work.registering&&work.enrollment_modality==CREDENTIAL_FINGER;}
/* Contact/lift tracking and authentication priority are separate. A small ROI
 * (2 / status 108) cannot authenticate; it must not starve palm/face or cancel
 * their work. It still counts as contact for enrollment and the success latch.
 * Qualified fingerprints keep priority; unknown capture state stays blocked. */
static int finger_blocks(void){return finger&&observed_finger!=0&&observed_finger!=2;}
static void finger_complete(BioJob *job,uint64_t ms);
static int user_uuid(const AppUser *u,unsigned kind){return kind==CREDENTIAL_FINGER?u->finger_uuid:kind==CREDENTIAL_FACE?u->face_uuid:u->palm_uuid;}
static int user_state(const AppUser *u,unsigned kind){return kind==CREDENTIAL_FINGER?u->finger_state:kind==CREDENTIAL_FACE?u->face_state:u->palm_state;}
static uint64_t face_next,face_unmatched,face_candidate_started;
static uint16_t face_candidate;
static int face_candidates,face_missing,face_held,face_latched;
static int enrollment_is_face(void){return work.enrollment_modality==CREDENTIAL_FACE;}
static int face_enrolling(void){return work.registering&&enrollment_is_face();}
static IrExposure exposure;
static IrExposureSample ir_sample;
static FaceIrPair face_pair;
static uint64_t palm_submitted_sequence;
static int stored_ir_target;
static char data_directory[384];
static ResgisRes_t progress;
static AppUser enrolling;
static uint16_t enrolling_template_uuid;
static uint64_t next_feature,session_started,last_event,unmatched_since;
static uint16_t candidate_uuid;static int candidate_count,latched;
static int hand_present,entry_streak,missing_streak,success_hold;
static unsigned feature_total,auth_total,held_total;
static void publish(void)
{
    work.verified_modalities=(success_hold?CREDENTIAL_PALM:0)|(face_held?CREDENTIAL_FACE:0)|(finger_held?CREDENTIAL_FINGER:0);
    pthread_mutex_lock(&lock);work.revision=shared.revision+1;shared=work;pthread_mutex_unlock(&lock);
}
static void finger_press_clear(void)
{
    memset(finger_press_features,0,sizeof(finger_press_features));
    memset(finger_press_sizes,0,sizeof(finger_press_sizes));finger_press_count=0;
    work.finger_enroll_samples=0;
}
static void finger_enroll_reset(void)
{
    finger_press_clear();finger_presses_done=0;finger_empty_since=finger_empty_last=0;
    work.finger_enroll_phase=FINGER_WAIT_PRESS;work.finger_enroll_press=1;
}
static void finger_enroll_observe(int found,uint64_t ms)
{
    if(!finger_enrolling())return;
    if(found<=0&&work.finger_enroll_phase==FINGER_COLLECT) {
        /* Do not combine an incomplete press with a later press. SDK receives
         * only whole ten-frame batches; roll back this press's UI progress. */
        finger_press_clear();work.progress=finger_presses_done*FINGER_ENROLL_PER_PRESS;
        work.finger_enroll_phase=FINGER_WAIT_RELEASE;
        snprintf(work.message,sizeof(work.message),"本轮采集未完成，请抬起手指并等待2秒");publish();
    }
    if(found!=0){finger_empty_since=finger_empty_last=0;}
    if(work.finger_enroll_phase==FINGER_WAIT_RELEASE&&found==0) {
        if(!finger_empty_since||ms<finger_empty_last||ms-finger_empty_last>500)finger_empty_since=ms;
        finger_empty_last=ms;
        if(ms>=finger_empty_since&&ms-finger_empty_since>=2000) {
            work.finger_enroll_phase=FINGER_WAIT_PRESS;
            work.finger_enroll_press=finger_presses_done+1;
            snprintf(work.message,sizeof(work.message),"请再次按压同一手指（第%d次，共5次）",finger_presses_done+1);
            printf("FINGER_ENROLL released press=%d empty_ms=%llu\n",finger_presses_done+1,(unsigned long long)(ms-finger_empty_since));publish();
        }
    } else if(work.finger_enroll_phase==FINGER_WAIT_PRESS&&found>0) {
        work.finger_enroll_phase=FINGER_COLLECT;
        snprintf(work.message,sizeof(work.message),"正在采集第%d次按压，请保持手指不动",finger_presses_done+1);publish();
    }
}
static int finger_enroll_restore(void)
{
    finger_enroll_reset();
    int rc=finger&&finger->set_enrolling?finger->set_enrolling(0):-1;
    if(rc){work.finger_ready=0;fprintf(stderr,"FINGER enrollment area restore failed\n");}
    return rc;
}
static void recognized(unsigned modality,uint64_t ms)
{
    ++work.success_sequence;work.success_modality=modality;work.success_ms=ms;
    work.success_relay_seconds=work.app_parameters.relay_seconds;
    if(trace_stall)printf("ACCESS_PUBLISH sequence=%u modality=%u uptime_us=%llu\n",
        work.success_sequence,modality,(unsigned long long)access_us());
    publish();
}
static uint64_t success_cooldown_remaining(uint64_t ms)
{
    uint64_t duration=(uint64_t)work.app_parameters.success_cooldown_seconds*1000;
    if(!work.success_sequence||!duration)return 0;
    uint64_t elapsed=ms>=work.success_ms?ms-work.success_ms:0;
    return elapsed<duration?duration-elapsed:0;
}
static void refresh(void)
{
    int n=store_list(&store,work.users,APP_MAX_USERS);
    if(n>=0)work.count=n;
    else {work.ready=0;snprintf(work.message,sizeof(work.message),"读取用户失败，请检查存储");}
}
static void event(enum access_event e,uint16_t uuid,const char *text)
{
    work.event=e;work.selected_id=uuid;++work.event_sequence;
    snprintf(work.message,sizeof(work.message),"%s",text);refresh();publish();
}
int access_submit(const AccessCommand *c)
{
    if(!c||c->count<0||c->count>APP_MAX_USERS)return -1;
    pthread_mutex_lock(&lock);
    if(count==8){pthread_mutex_unlock(&lock);return -1;}
    queue[tail]=*c;tail=(tail+1)%8;++count;pthread_mutex_unlock(&lock);return 0;
}
void access_enable_recognition(int enabled)
{
    pthread_mutex_lock(&lock);
    if(auth_enabled!=!!enabled)++auth_epoch;
    auth_enabled=!!enabled;pthread_mutex_unlock(&lock);
}
int access_snapshot(AccessSnapshot *s,unsigned previous)
{
    pthread_mutex_lock(&lock);int changed=shared.revision!=previous;
    if(changed)*s=shared;
    pthread_mutex_unlock(&lock);return changed;
}
const char *access_data_directory(void){return data_directory;}
static int template_exists(unsigned kind,uint16_t id)
{
    return kind==CREDENTIAL_FINGER?(finger?finger->exists(id):-1):kind==CREDENTIAL_FACE?(face?face->exists(id):-1):(palm?palm->exists(id):-1);
}
static int template_complete(unsigned kind,uint16_t id)
{
    return kind==CREDENTIAL_FINGER?(finger?finger->complete(id):-1):kind==CREDENTIAL_FACE?(face?face->complete(id):-1):(palm?palm->complete(id):-1);
}
static int delete_template_kind(unsigned kind,uint16_t id)
{
    if(id>=APP_TEMPLATE_SLOTS)return -1;
    int exists=template_exists(kind,id);if(exists<0)return -1;
    if(exists&&(kind==CREDENTIAL_FINGER?finger->remove(id):kind==CREDENTIAL_FACE?face->remove(id):palm->remove(id)))return -1;
    return template_exists(kind,id)==0?0:-1;
}
static int templates_available(const AppUser *u)
{
    return (u->palm_uuid<0||palm)&&(u->face_uuid<0||face)&&(u->finger_uuid<0||finger);
}
static int delete_all_templates(uint16_t local_id)
{
    AppUser u;if(store_get(&store,local_id,&u))return -1;
    if(u.palm_uuid>=0&&delete_template_kind(CREDENTIAL_PALM,u.palm_uuid))return -1;
    if(u.face_uuid>=0&&delete_template_kind(CREDENTIAL_FACE,u.face_uuid))return -1;
    if(u.finger_uuid>=0&&delete_template_kind(CREDENTIAL_FINGER,u.finger_uuid))return -1;
    return 0;
}
static int reset_old_templates(void)
{
    if(!palm||!face)return -1;
    if(palm->clear()||face->clear())return -1;
    for(int k=0;k<2;k++) {
        uint16_t ids[APP_TEMPLATE_SLOTS];int n=k?face->ids(ids,APP_TEMPLATE_SLOTS):palm->ids(ids,APP_TEMPLATE_SLOTS);
        if(n<0||n>APP_TEMPLATE_SLOTS)return -1;
        for(int i=0;i<n;i++)if(delete_template_kind(k?CREDENTIAL_FACE:CREDENTIAL_PALM,ids[i]))return -1;
        if((k?face->ids(ids,APP_TEMPLATE_SLOTS):palm->ids(ids,APP_TEMPLATE_SLOTS))!=0)return -1;
    }
    if(store_finish_templates_reset(&store))return -1;
    printf("ACCESS old user data and both SDK template databases reset for schema 3\n");return 0;
}
static int recover(void)
{
    if(finger) {
        int reset=store_fingerprint_reset_pending(&store);if(reset<0)return -1;
        if(reset) {
            uint16_t ids[APP_TEMPLATE_SLOTS];int n=finger->ids(ids,APP_TEMPLATE_SLOTS);
            if(n<0||n>APP_TEMPLATE_SLOTS||finger->clear())return -1;
            for(int i=0;i<n;i++)if(delete_template_kind(CREDENTIAL_FINGER,ids[i]))return -1;
            if(finger->ids(ids,APP_TEMPLATE_SLOTS)!=0||store_finish_fingerprint_reset(&store))return -1;
            printf("FINGER_MIGRATION ROI contract applied; removed=%d old fingerprint templates; face/palm retained\n",n);
        }
    }
    int reset=store_templates_reset_pending(&store);
    if(reset<0||(reset&&reset_old_templates()))return -1;
    PendingOperation pending[APP_MAX_USERS];int n=store_pending(&store,pending,APP_MAX_USERS);
    if(n<0)return -1;
    for(int i=0;i<n;i++) {
        uint16_t id=pending[i].local_id;unsigned kind=pending[i].modality;
        if(pending[i].deleting) {
            AppUser u;if(store_get(&store,id,&u))return -1;
            if(!templates_available(&u))continue;
            if(delete_all_templates(id)||store_finish_delete(&store,id))return -1;
        } else {
            AppUser u;if(store_get(&store,id,&u))return -1;
            int template_uuid=pending[i].template_uuid;
            if(template_uuid<0||template_uuid>=APP_TEMPLATE_SLOTS||
               template_uuid!=user_uuid(&u,kind))return -1;
            if((kind==CREDENTIAL_PALM&&!palm)||(kind==CREDENTIAL_FACE&&!face)||(kind==CREDENTIAL_FINGER&&!finger))continue;
            int complete=template_complete(kind,template_uuid);
            if(complete<0)return -1;
            if(complete==1) {
                if(store_finish_enroll_kind(&store,id,-1,kind))return -1;
            } else if(delete_template_kind(kind,template_uuid)||store_cancel_enroll_kind(&store,id,"interrupted enrollment",kind))return -1;
        }
        printf("ACCESS recovered local_id=%u modality=%u template_uuid=%d operation=%s\n",id,kind,pending[i].template_uuid,pending[i].deleting?"delete":"enroll");
    }
    refresh();uint8_t known[3][APP_TEMPLATE_SLOTS]={{0}};
    const unsigned kinds[]={CREDENTIAL_PALM,CREDENTIAL_FACE,CREDENTIAL_FINGER};
    const int enabled[]={!!palm,!!face,!!finger};
    for(int i=0;i<work.count;i++) {
        AppUser *u=&work.users[i];
        for(int k=0;k<3;k++) {
            int id=user_uuid(u,kinds[k]);if(id<0)continue;
            if(id>=APP_TEMPLATE_SLOTS)return -1;
            if(enabled[k]&&user_state(u,kinds[k])==CRED_REGISTERED) {
                int exists=template_exists(kinds[k],id);if(exists<0)return -1;
                if(!exists){if(store_mark_missing_kind(&store,u->local_id,kinds[k]))return -1;continue;}
            }
            known[k][id]=1;
        }
    }
    work.orphan_count=0;
    for(int k=0;k<3;k++) {
        if(!enabled[k])continue;
        uint16_t ids[APP_TEMPLATE_SLOTS];
        n=k==2?finger->ids(ids,APP_TEMPLATE_SLOTS):k==1?face->ids(ids,APP_TEMPLATE_SLOTS):palm->ids(ids,APP_TEMPLATE_SLOTS);
        if(n<0||n>APP_TEMPLATE_SLOTS)return -1;
        for(int i=0;i<n;i++)if(ids[i]>=APP_TEMPLATE_SLOTS||!known[k][ids[i]])++work.orphan_count;
    }
    refresh();return 0;
}
int access_open(const char *directory,const PalmBackend *backend)
{
    return access_open_biometrics(directory,backend,NULL);
}
int access_open_biometrics(const char *directory,const PalmBackend *backend,const FaceBackend *face_backend)
{return access_open_modalities(directory,backend,face_backend,NULL);}
int access_open_modalities(const char *directory,const PalmBackend *backend,const FaceBackend *face_backend,const FingerBackend *finger_backend)
{
    trace_stall=getenv("KY50_TRACE_STALL")!=NULL;
    face=face_backend;finger=finger_backend;
    observed_finger=finger?-1:0;finger_missing=finger_held=finger_latched=finger_candidates=0;
    finger_observed_ms=finger_observed_id=finger_submitted_id=finger_unmatched=0;
    ir_exposure_init(&exposure,NULL,-1);stored_ir_target=-1;
    ir_sample=(IrExposureSample){0};face_pair=(FaceIrPair){0};palm_submitted_sequence=0;
    memset(&work,0,sizeof(work));memset(&progress,0,sizeof(progress));
    palm=backend;snprintf(data_directory,sizeof(data_directory),"%s",directory);
    if(strlen(directory)+32>=sizeof(data_directory))return -1;
    if(mkdir(directory,0700)&&errno!=EEXIST)goto fail;
    char path[448];snprintf(path,sizeof(path),"%s/avatars",directory);if(mkdir(path,0700)&&errno!=EEXIST)goto fail;
    snprintf(path,sizeof(path),"%s/users.sqlite3",directory);if(store_open(&store,path))goto fail;
    work.ready=1;work.palm_ready=!!palm;work.face_ready=!!face;work.finger_ready=!!finger;
    finger_enroll_reset();
    if(finger&&(!finger->set_enrolling||finger->set_enrolling(0)))work.finger_ready=0;
    if(recover()) {
        /* Keep incomplete operations visible and reserved. Recognition stays
         * disabled until consistent storage is available. */
        work.ready=work.face_ready=work.palm_ready=work.finger_ready=0;snprintf(work.message,sizeof(work.message),"数据库操作待恢复，请重启重试");
    } else snprintf(work.message,sizeof(work.message),work.orphan_count?"发现未绑定模板，已保留":"用户数据已就绪");
    if(palm&&palm->parameters_get&&palm->parameters_set&&
       !palm->parameters_get(&work.parameters)&&palm_parameters_valid(&work.parameters)) {
        PalmParameters saved=work.parameters;
        int rc=store_parameters_load(&store,&saved);
        if(rc<0||(rc==0&&palm->parameters_set(&saved))) {
            work.palm_ready=0;
            snprintf(work.parameters_message,sizeof(work.parameters_message),"参数恢复失败，请重启检查存储");
        } else {
            work.parameters=saved;work.parameters_ready=1;
            snprintf(work.parameters_message,sizeof(work.parameters_message),"调整后点击保存");
            printf("ACCESS parameters loaded source=%s hand=%d auth=%d dynamic=%d\n",
                rc==1?"defaults":"database",saved.hand_threshold,saved.auth_threshold,saved.dynamic_level);
        }
    } else snprintf(work.parameters_message,sizeof(work.parameters_message),"算法未就绪，参数不可用");
    if(face&&face->parameters_get&&face->parameters_set&&
       !face->parameters_get(&work.face_parameters)&&face_parameters_valid(&work.face_parameters)) {
        FaceParameters saved=work.face_parameters;
        int rc=store_face_parameters_load(&store,&saved);
        if(rc<0||(rc==0&&face->parameters_set(&saved))) {
            work.face_ready=0;
            snprintf(work.face_parameters_message,sizeof(work.face_parameters_message),"人脸参数恢复失败，请重启检查存储");
        } else {
            work.face_parameters=saved;work.face_parameters_ready=1;
            snprintf(work.face_parameters_message,sizeof(work.face_parameters_message),"保存后立即生效，重启后保留");
            printf("ACCESS face parameters loaded source=%s detection=%d quality=%d/%d liveness=%d/%d auth=%d filter=%d\n",
                rc==1?"defaults":"database",saved.detection_threshold,saved.quality_enable,saved.quality_threshold,
                saved.liveness_enable,saved.liveness_threshold,saved.auth_threshold,saved.filter_threshold);
        }
    } else snprintf(work.face_parameters_message,sizeof(work.face_parameters_message),"人脸算法未就绪，参数不可用");
    int config_rc=app_config_load(directory,&work.app_parameters);
    if(config_rc<0||(config_rc==1&&app_config_save(directory,&work.app_parameters))) {
        work.ready=0;
        snprintf(work.app_parameters_message,sizeof(work.app_parameters_message),"主应用配置读取或保存失败");
        snprintf(work.message,sizeof(work.message),"主应用配置异常，请检查 app.conf");
    } else {
        work.app_parameters_ready=1;
        snprintf(work.app_parameters_message,sizeof(work.app_parameters_message),"保存后立即生效，重启后保留");
    }
    printf("ACCESS departure palm=%d face=%d consecutive misses relay_seconds=%d match_confirmations=%d success_cooldown_seconds=%d strong_liveness=%d config=%s/app.conf ready=%d\n",
        work.app_parameters.palm_leave_frames,work.app_parameters.face_leave_frames,work.app_parameters.relay_seconds,work.app_parameters.match_confirmations,work.app_parameters.success_cooldown_seconds,work.app_parameters.strong_liveness,directory,work.app_parameters_ready);
    next_feature=session_started=last_event=unmatched_since=0;candidate_count=latched=0;
    hand_present=entry_streak=missing_streak=success_hold=0;
    enrolling_template_uuid=UINT16_MAX;
    face_next=face_unmatched=0;face_candidates=face_missing=face_held=face_latched=0;
    feature_total=auth_total=held_total=0;
    publish();return 0;
fail:
    work.ready=work.palm_ready=work.face_ready=work.finger_ready=0;snprintf(work.message,sizeof(work.message),"用户数据库不可用，请检查存储");publish();return -1;
}
static int cancel(const char *reason)
{
    if(!work.registering)return 0;
    uint16_t id=enrolling.local_id;
    work.registering=0;work.progress=work.total=0;progress=(ResgisRes_t){0};
    unsigned kind=work.enrollment_modality;
    if(kind==CREDENTIAL_FINGER){if(finger)finger->clear();finger_enroll_restore();}else if(enrollment_is_face()){if(face)face->clear();}else if(palm)palm->clear();
    if(delete_template_kind(kind,enrolling_template_uuid)||store_cancel_enroll_kind(&store,id,reason,kind)) {
        work.palm_ready=work.face_ready=0;event(ACCESS_EVENT_ERROR,id,"取消待恢复，请重启重试");return -1;
    }
    snprintf(work.message,sizeof(work.message),"已取消注册");refresh();publish();return 0;
}
int access_start_async(void)
{
    if(bio_worker_start_modalities(palm,face,finger))return -1;
    async_enabled=1;palm_quality_ms=face_quality_ms=0;
    observed_hand=observed_face=-1;observed_hand_ms=observed_face_ms=maintenance_ms=0;return 0;
}
void access_close(void)
{
    if(async_enabled){bio_worker_stop();async_enabled=0;}
    cancel("application shutdown");store_close(&store);palm=NULL;face=NULL;finger=NULL;
    ir_exposure_init(&exposure,NULL,-1);
}
void access_set_exposure_backend(const IrExposureBackend *backend)
{
    stored_ir_target=-1;
    int rc=store.db?store_ir_target_load(&store,&stored_ir_target):-1;
    if(rc<0)fprintf(stderr,"IR_AE saved target unavailable; use current ISP default\n");
    ir_exposure_init(&exposure,backend,stored_ir_target);
    printf("IR_AE saved_target=%d; apply only after hand detection\n",stored_ir_target);
}
void access_set_face_pair(const FaceIrPair *pair){face_pair=pair?*pair:(FaceIrPair){0};}
void access_set_ir_sample(const IrExposureSample *sample)
{
    ir_sample=sample?*sample:(IrExposureSample){0};
}
void access_detection_gap(void){finger_enroll_observe(-1,0);observed_finger=finger?-1:0;finger_observed_ms=0;finger_missing=finger_candidates=0;finger_unmatched=0;observed_hand=observed_face=-1;observed_hand_ms=observed_face_ms=0;if(async_enabled)bio_worker_cancel(0);palm_quality_ms=face_quality_ms=0;entry_streak=missing_streak=0;face_candidates=face_missing=0;face_unmatched=0;}
void access_exposure_reset(void){ir_exposure_reset(&exposure);access_detection_gap();}
void access_log_pipeline(void)
{
    printf("PALM_PIPELINE features=%u auth=%u held=%u present=%d entry=%d missing=%d success_hold=%d\n",
        feature_total,auth_total,held_total,hand_present,entry_streak,missing_streak,success_hold);
}
static void set_parameters(const PalmParameters *p)
{
    const char *message="已保存，立即生效";int rc=-1;
    if(!work.ready||!work.parameters_ready||!palm||!palm->parameters_set)message="算法或存储不可用";
    else if(work.registering)message="注册进行中，请完成后再修改";
    else if(!palm_parameters_valid(p))message="阈值范围 0.00-1.00，等级范围 0-4";
    else {
        PalmParameters old=work.parameters;
        int applied=palm->parameters_set(p);
        if(applied||store_parameters_save(&store,p)) {
            message="保存失败，已恢复原参数";
            if(palm->parameters_set(&old)) {
                work.parameters_ready=work.palm_ready=0;
                message="参数恢复失败，请重启";
            }
        } else {
            work.parameters=*p;rc=0;
            candidate_count=latched=0;unmatched_since=0;
            printf("ACCESS parameters saved hand=%d auth=%d dynamic=%d\n",
                p->hand_threshold,p->auth_threshold,p->dynamic_level);
        }
    }
    work.parameters_result=rc;++work.parameters_sequence;
    snprintf(work.parameters_message,sizeof(work.parameters_message),"%s",message);publish();
}
static void set_face_parameters(const FaceParameters *p)
{
    const char *message="已保存，立即生效";int rc=-1;
    if(!work.ready||!work.face_parameters_ready||!face||!face->parameters_set)message="人脸算法或存储不可用";
    else if(work.registering)message="注册进行中，请完成后再修改";
    else if(!face_parameters_valid(p))message="人脸参数超出允许范围";
    else {
        FaceParameters old=work.face_parameters;
        if(face->parameters_set(p)||store_face_parameters_save(&store,p)) {
            message="保存失败，已恢复原参数";
            if(face->parameters_set(&old)) {
                work.face_parameters_ready=work.face_ready=0;
                message="人脸参数恢复失败，请重启";
            }
        } else {
            work.face_parameters=*p;rc=0;
            face_candidates=face_latched=0;face_unmatched=0;
            /* An already successful face remains held until configured departure. */
            printf("ACCESS face parameters saved detection=%d quality=%d/%d liveness=%d/%d auth=%d filter=%d\n",
                p->detection_threshold,p->quality_enable,p->quality_threshold,
                p->liveness_enable,p->liveness_threshold,p->auth_threshold,p->filter_threshold);
        }
    }
    work.face_parameters_result=rc;++work.face_parameters_sequence;
    snprintf(work.face_parameters_message,sizeof(work.face_parameters_message),"%s",message);publish();
}
static void set_app_parameters(const AppParameters *p)
{
    const char *message="已保存，立即生效";int rc=-1;
    if(!work.ready||!work.app_parameters_ready)message="主应用配置不可用";
    else if(work.registering)message="注册进行中，请完成后再修改";
    else if(!app_parameters_valid(p))message="参数超出允许范围，请检查";
    else if(p->strong_liveness&&p->strong_liveness!=work.app_parameters.strong_liveness&&(!face||!face->live_ir||!work.face_ready))message="红外人脸活体接口不可用";
    else if(app_config_save(data_directory,p))message="保存失败，原参数仍生效";
    else {
        work.app_parameters=*p;rc=0;
        candidate_count=face_candidates=finger_candidates=0;face_candidate_started=0;
        /* Start a fresh streak under the new thresholds; saving never unlocks auth. */
        missing_streak=face_missing=finger_missing=0;
        printf("ACCESS application saved palm=%d face=%d relay_seconds=%d match_confirmations=%d success_cooldown_seconds=%d strong_liveness=%d config=%s/app.conf\n",
            p->palm_leave_frames,p->face_leave_frames,p->relay_seconds,p->match_confirmations,p->success_cooldown_seconds,p->strong_liveness,data_directory);
    }
    work.app_parameters_result=rc;++work.app_parameters_sequence;
    snprintf(work.app_parameters_message,sizeof(work.app_parameters_message),"%s",message);publish();
}
void access_poll(uint64_t ms)
{
    async_quiescing=0;
    pthread_mutex_lock(&lock);int recognize=auth_enabled;unsigned epoch=auth_epoch;pthread_mutex_unlock(&lock);
    if(async_enabled&&epoch!=observed_auth_epoch){bio_worker_cancel(0);observed_auth_epoch=epoch;}
    if(async_enabled&&!recognize&&!work.registering){bio_worker_cancel(CREDENTIAL_FACE);bio_worker_cancel(CREDENTIAL_FINGER);}
    if(!recognize){finger_candidates=0;finger_unmatched=0;candidate_count=latched=0;unmatched_since=0;face_candidates=face_latched=0;face_unmatched=0;}
    if(*work.recognition&&(!recognize||ms-last_event>2500)) {
        work.matched=0;work.recognition[0]=0;publish();
    }
    if(work.registering&&ms-session_started>120000) {
        if(async_enabled){async_quiescing=1;bio_worker_cancel(0);if(bio_worker_busy())return;}
        cancel("registration timeout");event(ACCESS_EVENT_ERROR,enrolling.local_id,"注册超时，请重试");
    }
    AccessCommand c;int have;
    if(async_enabled) {
        pthread_mutex_lock(&lock);int pending=count>0;pthread_mutex_unlock(&lock);
        if(pending){async_quiescing=1;bio_worker_cancel(0);if(bio_worker_busy())return;}
    }
    pthread_mutex_lock(&lock);have=count>0;
    if(have){c=queue[head];head=(head+1)%8;--count;}
    pthread_mutex_unlock(&lock);if(!have)return;
    if(c.type==ACCESS_PARAMETERS){set_parameters(&c.parameters);return;}
    if(c.type==ACCESS_FACE_PARAMETERS){set_face_parameters(&c.face_parameters);return;}
    if(c.type==ACCESS_APP_PARAMETERS){set_app_parameters(&c.app_parameters);return;}
    if(!work.ready){event(ACCESS_EVENT_ERROR,0,"用户数据库不可用");return;}
    work.busy=1;publish();
    if(c.type==ACCESS_CREATE) {
        AppUser u={0};int rc=store_create(&store,c.employee_id,c.name,&u);
        work.busy=0;
        event(rc?ACCESS_EVENT_ERROR:ACCESS_EVENT_CREATED,u.local_id,rc==-2?"工号已存在，请重新输入":rc==-3?"当前用户容量已满":rc?"用户保存失败，请重试":"用户已保存，请选择注册类型");
    } else if(c.type==ACCESS_ENROLL) {
        if(cancel("new registration"))goto finished;
        unsigned kind=c.modality?c.modality:CREDENTIAL_PALM;
        int is_face=kind==CREDENTIAL_FACE,is_finger=kind==CREDENTIAL_FINGER;
        if((kind!=CREDENTIAL_PALM&&!is_face&&!is_finger)||(is_finger?(!work.finger_ready||!finger):is_face?(!work.face_ready||!face):(!work.palm_ready||!palm))) {
            work.busy=0;event(ACCESS_EVENT_ERROR,c.local_id,is_finger?"指纹算法不可用":is_face?"人脸算法不可用":"掌静脉算法不可用");return;
        }
        if(store_get(&store,c.local_id,&enrolling)||enrolling.deleting) {
            work.busy=0;event(ACCESS_EVENT_ERROR,c.local_id,"用户不存在或正在删除");return;
        }
        if(user_state(&enrolling,kind)==CRED_REGISTERED||
           user_state(&enrolling,kind)==CRED_PENDING) {
            work.busy=0;event(ACCESS_EVENT_ERROR,c.local_id,"该方式已注册或模板不可用，请勿重复注册");return;
        }
        int template_uuid=is_finger?finger->pick_free():is_face?(face->pick_free?face->pick_free():-1):(palm->pick_free?palm->pick_free():-1);
        if(template_uuid<0||template_uuid>=APP_TEMPLATE_SLOTS||template_exists(kind,template_uuid)!=0) {
            work.busy=0;event(ACCESS_EVENT_ERROR,c.local_id,"该方式模板库已满或编号查询失败");return;
        }
        int capacity=is_finger?finger->capacity():is_face?face->capacity():palm->capacity();
        if(capacity<=0||(is_finger&&capacity!=FINGER_ENROLL_TOTAL)||(is_finger?finger->clear():is_face?face->clear():palm->clear())) {
            work.busy=0;event(ACCESS_EVENT_ERROR,c.local_id,"无法开始注册，请重试");return;
        }
        if(is_finger&&(!finger->set_enrolling||finger->set_enrolling(1))) {
            finger_enroll_restore();work.busy=0;event(ACCESS_EVENT_ERROR,c.local_id,"指纹注册参数设置失败");return;
        }
        if(store_prepare_enroll_kind(&store,c.local_id,kind,template_uuid)) {
            if(is_finger)finger_enroll_restore();
            work.busy=0;event(ACCESS_EVENT_ERROR,c.local_id,"无法开始注册，请重试");return;
        }
        if(is_finger)finger_enroll_reset();
        progress=(ResgisRes_t){.status=STATUS_IN_REGISING,.total=capacity};
        enrolling_template_uuid=template_uuid;
        printf("ACCESS enroll begin local_id=%u modality=%s template_uuid=%u\n",c.local_id,is_finger?"fingerprint":is_face?"face":"palm",enrolling_template_uuid);
        work.enrollment_modality=kind;
        work.registering=1;work.selected_id=c.local_id;work.progress=0;work.total=capacity;work.status=STATUS_FAILED;
        work.matched=0;work.recognition[0]=0;session_started=ms;next_feature=face_next=0;
        snprintf(work.message,sizeof(work.message),"%s",is_finger?"请将同一手指放在指纹采集区":is_face?"请正对摄像头，并移开手掌":"请将手掌放入画面，手指朝上");refresh();
    } else if(c.type==ACCESS_CANCEL) {
        cancel("user cancelled");
    } else if(c.type==ACCESS_DELETE) {
        if(cancel("user deletion"))goto finished;
        int failed=0,deleted=0;
        for(int i=0;i<c.count;i++) {
            AppUser u;if(store_get(&store,c.ids[i],&u)||!templates_available(&u)){failed++;continue;}
            if(store_prepare_delete(&store,u.local_id)||delete_all_templates(u.local_id)||store_finish_delete(&store,u.local_id)){failed++;continue;}
            ++deleted;printf("ACCESS deleted local_id=%u\n",u.local_id);
        }
        char text[192];snprintf(text,sizeof(text),failed?"已删除 %d 人，%d 人待重试":"已删除 %d 人",deleted,failed);
        work.busy=0;event(failed?ACCESS_EVENT_ERROR:ACCESS_EVENT_DELETED,0,text);return;
    }
finished:
    work.busy=0;publish();
}
static const char *quality(int s)
{
    switch(s) {
    case STATUS_TOO_LEFT:return "请向右移动手掌";case STATUS_TOO_RIGHT:return "请向左移动手掌";
    case STATUS_TOO_TOP:return "请向下移动手掌";case STATUS_TOO_BOT:return "请向上移动手掌";
    case STATUS_TOO_CLOSE:return "手掌太近，请稍微远离";case STATUS_TOO_FAR:return "手掌太远，请靠近";
    case STATUS_TOO_BRIGHT:return "手掌过亮，请调整位置";case STATUS_TOO_DARK:return "手掌过暗，请改善红外照明";
    case STATUS_TOO_BLUR:return "请保持手掌清晰稳定";case STATUS_INVALID_ANGLE:return "请让手指朝上";
    case STATUS_WEAR_GLOVE:return "请摘下手套";case STATUS_IN_REGISING_DYNAMIC:return "请稍微转动手掌";
    case STATUS_IMG_NOT_SAME:return "图像状态异常，请重试";
    default:return "请将手掌放入画面";
    }
}
static void palm_auth_complete(int auth_result,const User_For_C_t *result,const IrExposureProof *proof,uint64_t ms)
{
    AppUser u;
    int match=auth_result>0 && !store_get_by_template(&store,CREDENTIAL_PALM,result->uuid,&u) && u.enabled&&!u.deleting&&u.palm_state==CRED_REGISTERED;
    if(match) {
        unmatched_since=0;
        if(!candidate_count||candidate_uuid!=result->uuid){candidate_uuid=result->uuid;candidate_count=1;}else ++candidate_count;
        if(trace_stall&&candidate_count==work.app_parameters.match_confirmations)
            printf("PALM_CONFIRM samples=%d event_wait_ms=%llu uptime_ms=%llu\n",candidate_count,
                (unsigned long long)success_cooldown_remaining(ms),(unsigned long long)ms);
        if(candidate_count>=work.app_parameters.match_confirmations&&!success_cooldown_remaining(ms)) {
            uint64_t record_begin=access_us();int record_result=store_record_match(&store,&u,result->targetDist,ms);
            trace_stage("palm","attendance",record_begin,record_result);
            if(record_result)snprintf(work.recognition,sizeof(work.recognition),"识别成功，记录保存失败");
            else snprintf(work.recognition,sizeof(work.recognition),"识别成功  %s\n工号 %s",*u.name?u.name:"未设置姓名",u.employee_id);
            work.matched=1;work.score=result->targetDist;last_event=ms;latched=success_hold=1;
            recognized(CREDENTIAL_PALM,ms);
            printf("ACCESS match local_id=%u palm_uuid=%u score=%.4f\n",u.local_id,result->uuid,result->targetDist);
            printf("PALM_SESSION success_hold features=%u auth=%u\n",feature_total,auth_total);
            int target=ir_exposure_success(&exposure,proof,ms);
            if(target>=1&&target!=stored_ir_target) {
                if(store_ir_target_save(&store,target))fprintf(stderr,"IR_AE success target persistence failed; kept in memory\n");
                else {stored_ir_target=target;printf("IR_AE persisted_success target=%d\n",target);}
            }
        }
    } else {
        candidate_count=0;
        if(!unmatched_since)unmatched_since=ms;
        if(ms-unmatched_since>2000&&ms-last_event>=3000) {
            if(store_record_unknown(&store,ms))fprintf(stderr,"ACCESS unmatched event storage failed\n");
            snprintf(work.recognition,sizeof(work.recognition),"未识别到已注册用户");work.matched=-1;last_event=ms;latched=1;publish();
        }
    }
}
static int palm_feature_complete(HandPoseRes_t *pose,uint8_t *feat,int result,int rgb_preview,
                                 const IrExposureSample *sample,const BioJob *combined,uint64_t ms)
{
    int found=1;
    /* Some exceptional SDK exits may not update the structure. Fail closed. */
    if(result!=STATUS_OK&&pose->status==STATUS_OK)pose->status=result;
    ir_exposure_feedback(&exposure,found,pose->status,ms,sample);
    if(pose->status!=STATUS_OK) {
        candidate_count=0;unmatched_since=0;
        if(work.registering&&work.enrollment_modality==CREDENTIAL_PALM&&work.status!=pose->status){work.status=pose->status;snprintf(work.message,sizeof(work.message),"%s",quality(pose->status));publish();}
        memset(feat,0,APP_FEATURE_BYTES);return 1;
    }
    int recognize;pthread_mutex_lock(&lock);recognize=auth_enabled;pthread_mutex_unlock(&lock);
    if(!work.registering&&!recognize){candidate_count=0;latched=0;}
    if((success_hold&&!work.registering)||!work.ready||!work.palm_ready||(!work.registering&&!recognize)||
       (work.registering&&!rgb_preview)||(!work.registering&&latched)||ms<next_feature) {
        memset(feat,0,APP_FEATURE_BYTES);return 1;
    }
    if(face_enrolling()||finger_enrolling()||finger_blocks()){memset(feat,0,APP_FEATURE_BYTES);return 1;}
    /* Sequential independent async images already bound sampling. Keep the
     * original pacing for enrollment and the synchronous compatibility path. */
    next_feature=ms+((async_enabled&&!work.registering)?0:180);
    if(work.registering) {
        User_For_C_t user={.uuid=enrolling_template_uuid,.isAdmin=enrolling.is_admin};
        app_utf8_copy(user.userName,sizeof(user.userName),enrolling.name);
        if(palm->enroll(&user,feat,&progress))progress.status=STATUS_REGIS_FAILED;
        work.status=progress.status;work.progress=progress.progress;work.total=progress.total;
        if(progress.status==STATUS_REGIS_SUCCESS) {
            uint16_t id=enrolling.local_id;work.registering=0;palm->clear();
            if(palm->complete(enrolling_template_uuid)!=1||store_finish_enroll(&store,id,pose->leftOrRight)) {
                /* Leave the durable operation pending for next-start recovery. */
                work.palm_ready=0;event(ACCESS_EVENT_ERROR,id,"注册保存待恢复，请重启重试");
            } else {event(ACCESS_EVENT_ENROLLED,id,"掌静脉注册成功");printf("ACCESS enrolled local_id=%u palm_uuid=%u progress=%d/%d\n",id,enrolling_template_uuid,progress.progress,progress.total);}
        } else if(progress.status==STATUS_REGIS_FAILED||progress.status==STATUS_REGIS_FAILED_ALREADY_REGIS||progress.status==STATUS_REGIS_FAILED_MULTIY_REGIS) {
            int result=progress.status;cancel("SDK enrollment rejected");
            event(ACCESS_EVENT_ERROR,enrolling.local_id,result==STATUS_REGIS_FAILED_ALREADY_REGIS?"该手掌已注册，请勿重复注册":result==STATUS_REGIS_FAILED_MULTIY_REGIS?"请只注册同一只手掌":"注册失败，请重试");
        } else {snprintf(work.message,sizeof(work.message),"%s",progress.status==STATUS_IN_REGISING_DYNAMIC?quality(progress.status):"正在采集，请适当调整手掌角度");publish();}
    } else if(!latched) {
        IrExposureProof proof=ir_exposure_proof(&exposure,sample);
        if(combined) {
            if(combined->auth_ran) {
                ++auth_total;palm_auth_complete(combined->auth_result,&combined->user,&combined->ir_proof,ms);
            }
        } else if(async_enabled) {
            BioJob job={.kind=CREDENTIAL_PALM,.stage=BIO_AUTH,.submitted_ms=ms,.ir_proof=proof,.ir_sample=*sample};
            memcpy(job.feature,feat,APP_FEATURE_BYTES);
            if(bio_worker_submit(&job,NULL,0,0,NULL)>0)++auth_total;
        } else {
            User_For_C_t user={0};++auth_total;
            uint64_t begin=access_us();int rc=palm->recognize(feat,&user);
            trace_stage("palm","sdk_auth",begin,rc);palm_auth_complete(rc,&user,&proof,ms);
        }
    }
    memset(feat,0,APP_FEATURE_BYTES);return 1;
}
static void palm_schedule(uint8_t *yuv,uint16_t w,uint16_t h,const HandPoseRes_t *pose,int rgb_preview,uint64_t ms)
{
    if(finger_blocks()||finger_enrolling()||async_quiescing||!hand_present||!palm||!work.ready||!work.palm_ready||
       (ir_sample.valid&&ir_sample.sequence<=palm_submitted_sequence))return;
    int recognize;pthread_mutex_lock(&lock);recognize=auth_enabled;pthread_mutex_unlock(&lock);
    int combined=!work.registering&&recognize&&!success_hold&&!latched&&ms>=next_feature;
    BioJob job={.kind=CREDENTIAL_PALM,.stage=combined?BIO_PALM_RECOGNIZE:BIO_FEATURE,
        .hand=*pose,.rgb_preview=rgb_preview,.submitted_ms=ms,.ir_sample=ir_sample,
        .ir_proof=ir_exposure_proof(&exposure,&ir_sample)};
    if(bio_worker_submit(&job,yuv,w,h,NULL)>0) {++feature_total;palm_submitted_sequence=ir_sample.sequence;}
}
int access_frame(uint8_t *yuv,uint16_t w,uint16_t h,HandPoseRes_t *pose,int found,int rgb_preview,uint64_t ms)
{
    if(!pose||found<0){entry_streak=missing_streak=0;candidate_count=0;return 0;}
    if(found<=0) {
        entry_streak=0;
        if(hand_present&&++missing_streak>=work.app_parameters.palm_leave_frames) {
            printf("PALM_SESSION left false_frames=%d success_hold=%d features=%u auth=%u held=%u\n",
                missing_streak,success_hold,feature_total,auth_total,held_total);
            hand_present=success_hold=0;candidate_count=latched=0;
            next_feature=unmatched_since=0;ir_exposure_reset(&exposure);
            publish();
        }
        ir_exposure_feedback(&exposure,found,pose->status,ms,&ir_sample);
        if(work.registering&&work.enrollment_modality==CREDENTIAL_PALM&&work.status!=STATUS_FAILED){work.status=STATUS_FAILED;snprintf(work.message,sizeof(work.message),"请将手掌放入画面");publish();}
        if(!work.registering&&work.matched&&ms-last_event>2500){work.matched=0;work.recognition[0]=0;publish();}
        return 0;
    }
    missing_streak=0;
    if(!hand_present) {
        if(++entry_streak<PALM_ENTER_FRAMES){pose->status=STATUS_FAILED;return 0;}
        hand_present=1;entry_streak=0;
        printf("PALM_SESSION entered true_frames=%d\n",PALM_ENTER_FRAMES);
    }
    /* A successful session keeps extracting features for live box color and
     * exposure feedback. Only auth is held. UI navigation, parameter edits
     * and elapsed time cannot unlock a successful hand still in view. */
    if(success_hold)++held_total;
    if(!palm){pose->status=STATUS_FAILED;return 1;}
    if(async_enabled) {
        palm_schedule(yuv,w,h,pose,rgb_preview,ms);
        pose->status=palm_quality_ms&&ms-palm_quality_ms<1000?palm_quality_status:-1;
        return 1;
    }
    if(finger_blocks()||finger_enrolling())return 1;
    uint8_t feat[APP_FEATURE_BYTES]={0};++feature_total;
    uint64_t begin=access_us();int result=palm->features(yuv,w,h,pose,feat);
    trace_stage("palm","feature",begin,result);
    return palm_feature_complete(pose,feat,result,rgb_preview,&ir_sample,NULL,ms);
}

static const char *face_quality(int status)
{
    switch(status) {
    case STATUS_FACE_LOW_QUALITY:return "请调整距离和光线，保持人脸清晰";
    case STATUS_FACE_LIVENESS_FAILED:return "活体检测未通过，请重试";
    case STATUS_FACE_ALIGN_FAILED:return "请正对摄像头";
    case STATUS_FACE_MODEL_FAILED:return "人脸算法运行失败";
    case STATUS_IMG_NOT_SAME:return "图像状态异常，请重试";
    default:return "请将人脸放入画面";
    }
}
static void face_auth_complete(int auth_result,const User_For_C_t *result_user,uint64_t ms)
{
    AppUser u;
    int matched=auth_result>0&&!store_get_by_template(&store,CREDENTIAL_FACE,result_user->uuid,&u)&&
                u.enabled&&!u.deleting&&u.face_state==CRED_REGISTERED;
    if(matched) {
        face_unmatched=0;
        if(!face_candidates||face_candidate!=result_user->uuid){face_candidate=result_user->uuid;face_candidates=1;face_candidate_started=ms;}else ++face_candidates;
        if(trace_stall&&face_candidates==work.app_parameters.match_confirmations)
            printf("FACE_CONFIRM samples=%d event_wait_ms=%llu uptime_ms=%llu\n",face_candidates,
                (unsigned long long)success_cooldown_remaining(ms),(unsigned long long)ms);
        if(face_candidates>=work.app_parameters.match_confirmations&&!success_cooldown_remaining(ms)) {
            uint64_t record_begin=access_us();int record_result=store_record_match_kind(&store,&u,result_user->targetDist,ms,CREDENTIAL_FACE);
            trace_stage("face","attendance",record_begin,record_result);
            if(record_result)
                snprintf(work.recognition,sizeof(work.recognition),"识别成功，记录保存失败");
            else snprintf(work.recognition,sizeof(work.recognition),"人脸识别成功  %s\n工号 %s",*u.name?u.name:"未设置姓名",u.employee_id);
            work.matched=1;work.score=result_user->targetDist;last_event=ms;face_held=face_latched=1;
            recognized(CREDENTIAL_FACE,ms);
            printf("ACCESS face match local_id=%u face_uuid=%u score=%.4f confirm_ms=%llu samples=%d\n",u.local_id,result_user->uuid,result_user->targetDist,(unsigned long long)(ms-face_candidate_started),face_candidates);
        }
    } else {
        face_candidates=0;if(!face_unmatched)face_unmatched=ms;
        if(ms-face_unmatched>2000&&ms-last_event>=3000) {
            if(store_record_unknown_kind(&store,ms,CREDENTIAL_FACE))fprintf(stderr,"ACCESS face event storage failed\n");
            snprintf(work.recognition,sizeof(work.recognition),"未识别到已注册用户");work.matched=-1;last_event=ms;face_latched=1;publish();
        }
    }
}
static void face_feature_complete(FacePoseRes_t *pose,uint8_t *feat,int result,const BioJob *combined,uint64_t ms)
{
    if(result!=STATUS_OK&&pose->status==STATUS_OK)pose->status=result;
    if(result!=STATUS_OK||pose->status!=STATUS_OK) {
        face_candidates=0;face_unmatched=0;
        if(face_enrolling()){work.status=pose->status;snprintf(work.message,sizeof(work.message),"%s",face_quality(pose->status));publish();}
    } else if(face_enrolling()) {
        User_For_C_t user={.uuid=enrolling_template_uuid,.isAdmin=enrolling.is_admin};
        app_utf8_copy(user.userName,sizeof(user.userName),enrolling.name);
        if(face->enroll(&user,feat,pose->sharpness,&progress))progress.status=STATUS_REGIS_FAILED;
        work.status=progress.status;work.progress=progress.progress;work.total=progress.total;
        if(progress.status==STATUS_REGIS_SUCCESS) {
            uint16_t id=enrolling.local_id;work.registering=0;face->clear();
            if(face->complete(enrolling_template_uuid)!=1||store_finish_enroll_kind(&store,id,-1,CREDENTIAL_FACE)) {
                work.face_ready=0;event(ACCESS_EVENT_ERROR,id,"人脸注册保存待恢复，请重启重试");
            } else {event(ACCESS_EVENT_ENROLLED,id,"人脸注册成功");printf("ACCESS face enrolled local_id=%u face_uuid=%u\n",id,enrolling_template_uuid);}
        } else if(progress.status==STATUS_REGIS_FAILED||progress.status==STATUS_REGIS_FAILED_ALREADY_REGIS||progress.status==STATUS_REGIS_FAILED_MULTIY_REGIS) {
            int status=progress.status;cancel("face enrollment rejected");
            event(ACCESS_EVENT_ERROR,enrolling.local_id,status==STATUS_REGIS_FAILED_ALREADY_REGIS?"该人脸已注册，请勿重复注册":"人脸注册失败，请重试");
        } else {snprintf(work.message,sizeof(work.message),"正在采集，请保持人脸稳定");publish();}
    } else {
        pthread_mutex_lock(&lock);int recognize=auth_enabled;pthread_mutex_unlock(&lock);
        if(!recognize){memset(feat,0,APP_FEATURE_BYTES);return;}
        if(combined)face_auth_complete(combined->auth_result,&combined->user,ms);
        else if(async_enabled) {
            BioJob job={.kind=CREDENTIAL_FACE,.stage=BIO_AUTH,.submitted_ms=ms};
            memcpy(job.feature,feat,APP_FEATURE_BYTES);
            bio_worker_submit(&job,NULL,0,0,NULL);
        } else {
            User_For_C_t user={0};uint64_t begin=access_us();int rc=face->recognize(feat,&user);
            trace_stage("face","sdk_auth",begin,rc);face_auth_complete(rc,&user,ms);
        }
    }
    memset(feat,0,APP_FEATURE_BYTES);
}
static void face_schedule(uint8_t *ir,uint8_t *nv12,uint16_t w,uint16_t h,FacePoseRes_t *pose,int rgb_preview,uint64_t ms)
{
    int recognize;pthread_mutex_lock(&lock);recognize=auth_enabled;pthread_mutex_unlock(&lock);
    if(finger_blocks()||(work.registering&&!face_enrolling()))return;
    if(!work.registering&&(!recognize||face_held||face_latched))return;
    if((work.registering&&!rgb_preview)||ms<face_next)return;
    if(async_enabled) {
        BioJob job={.kind=CREDENTIAL_FACE,.stage=work.registering?BIO_FEATURE:BIO_FACE_RECOGNIZE,.face=*pose,
            .rgb_preview=rgb_preview,.submitted_ms=ms,.ir_sample=ir_sample,.face_pair=face_pair,
            .require_ir_liveness=!work.registering&&work.app_parameters.strong_liveness};
        if(!async_quiescing&&bio_worker_submit(&job,nv12,w,h,ir)>0)face_next=ms+180;
        return;
    }
    face_next=ms+180;
    uint8_t feat[APP_FEATURE_BYTES]={0};
    uint64_t begin=access_us();int result=face->features(nv12,w,h,pose,feat);
    trace_stage("face","feature",begin,result);
    if(!work.registering&&work.app_parameters.strong_liveness&&result==STATUS_OK&&pose->status==STATUS_OK) {
        BioJob combined={.auth_ran=1,.require_ir_liveness=1};
        combined.auth_result=face->recognize(feat,&combined.user);
        if(combined.auth_result>0) {
            size_t y=(size_t)w*h;uint8_t *copy=NULL;
            if(ir&&w&&h&&!(w&1)&&!(h&1))copy=malloc(y*3/2);
            if(copy){memcpy(copy,ir,y);memset(copy+y,128,y/2);}
            combined.ir_live_result=bio_face_live_ir(face,copy,w,h,pose,&face_pair);
            if(copy){memset(copy,0,y*3/2);free(copy);}
            if(combined.ir_live_result!=1)result=pose->status=STATUS_FACE_LIVENESS_FAILED;
        }
        face_feature_complete(pose,feat,result,&combined,ms);
    } else face_feature_complete(pose,feat,result,NULL,ms);
}

static void face_frame(uint8_t *ir,uint8_t *nv12,uint16_t w,uint16_t h,FacePoseRes_t *pose,int found,
                       int raw_hand,int rgb_preview,uint64_t ms)
{
    if(found<=0) {
        face_candidates=0;face_unmatched=0;
        if(found<0)face_missing=0;
        else if(face_missing<work.app_parameters.face_leave_frames&&++face_missing>=work.app_parameters.face_leave_frames){
            face_held=face_latched=0;face_next=0;publish();
        }
    } else face_missing=0;
    /* Unknown IR is not permission to fall back to face recognition. Drawing
     * is independent and still uses the raw RGB Pose result. */
    if(finger_blocks()||raw_hand!=0||found<=0||!face||!work.face_ready||!work.ready) {
        face_candidates=0;face_unmatched=0;
        if(face_enrolling()) {
            snprintf(work.message,sizeof(work.message),"%s",raw_hand>0?"请移开手掌后注册人脸":raw_hand<0?"红外采集异常，请稍候":"请将人脸放入画面");publish();
        }
        return;
    }
    face_schedule(ir,nv12,w,h,pose,rgb_preview,ms);
}

void access_observe_hand(int found,uint64_t ms)
{
    observed_hand=found;observed_hand_ms=ms;
    if(async_enabled) {
        if(found<=0){bio_worker_cancel(CREDENTIAL_PALM);palm_quality_ms=0;}
        if(found!=0){bio_worker_cancel(CREDENTIAL_FACE);face_quality_ms=0;}
    }
}
int access_consume_results(uint64_t ms)
{
    if(!async_enabled)return 0;
    pthread_mutex_lock(&lock);
    int recognize=auth_enabled,pending=count>0;unsigned epoch=auth_epoch;
    pthread_mutex_unlock(&lock);
    if(epoch!=observed_auth_epoch){bio_worker_cancel(0);observed_auth_epoch=epoch;}
    /* A queued config/user command invalidates old work before it is applied. */
    if(pending||async_quiescing){bio_worker_cancel(0);return 0;}
    BioJob result;
    if(bio_worker_take(&result)) {
        int valid=!async_quiescing&&ms>=result.submitted_ms&&ms-result.submitted_ms<=1500&&
            observed_hand_ms&&ms>=observed_hand_ms&&ms-observed_hand_ms<=250;
        if(result.kind==CREDENTIAL_FINGER)valid=!async_quiescing&&ms>=result.submitted_ms&&ms-result.submitted_ms<=1500&&
            observed_finger==1&&finger_observed_ms&&ms>=finger_observed_ms&&ms-finger_observed_ms<=250;
        else if(result.kind==CREDENTIAL_PALM)valid=valid&&!finger_blocks()&&observed_hand>0&&hand_present;
        else valid=valid&&!finger_blocks()&&observed_hand==0&&observed_face>0&&
            observed_face_ms&&ms>=observed_face_ms&&ms-observed_face_ms<=250;
        if(trace_stall)printf("BIO_RESULT modality=%s valid=%d wait_us=%llu age_ms=%llu\n",
            result.kind==CREDENTIAL_FINGER?"fingerprint":result.kind==CREDENTIAL_FACE?"face":"palm",valid,
            (unsigned long long)(access_us()-result.completed_us),(unsigned long long)(ms-result.submitted_ms));
        if(valid&&result.stage!=BIO_AUTH) {
            if(result.kind==CREDENTIAL_FINGER)finger_complete(&result,ms);
            else if(result.kind==CREDENTIAL_PALM) {
                palm_feature_complete(&result.hand,result.feature,result.result,result.rgb_preview,&result.ir_sample,
                    result.stage==BIO_PALM_RECOGNIZE?&result:NULL,ms);
                palm_quality_status=result.hand.status;palm_quality_ms=ms;
            } else {
                face_feature_complete(&result.face,result.feature,result.result,
                    result.stage==BIO_FACE_RECOGNIZE?&result:NULL,ms);
                face_quality_status=result.face.status;face_quality_sharpness=result.face.sharpness;face_quality_ms=ms;
            }
        } else if(valid&&recognize&&!work.registering&&result.stage==BIO_AUTH) {
            if(result.kind==CREDENTIAL_PALM)palm_auth_complete(result.result,&result.user,&result.ir_proof,ms);
            else face_auth_complete(result.result,&result.user,ms);
        }
        memset(&result,0,sizeof(result));
        return 1;
    }
    return 0;
}
void access_idle_maintenance(uint64_t ms)
{
    if(!store.db||work.registering||finger_blocks()||observed_hand!=0||observed_face!=0||
       (async_enabled&&bio_worker_busy())||ms<maintenance_ms)return;
    maintenance_ms=ms+1000;
    store_checkpoint(&store,0);
}
void access_biometrics_frame(uint8_t *ir,uint8_t *nv12,uint16_t w,uint16_t h,
                            HandPoseRes_t *hand,int hand_found,FacePoseRes_t *pose,int face_found,
                            int rgb_preview,uint64_t ms)
{
    HandPoseRes_t raw_hand=*hand;
    access_observe_hand(hand_found,ms);
    observed_face=face_found;observed_face_ms=ms;
    if(async_enabled) {
        /* Navigation can occur while the two Pose calls are running. */
        pthread_mutex_lock(&lock);unsigned epoch=auth_epoch;pthread_mutex_unlock(&lock);
        if(epoch!=observed_auth_epoch){bio_worker_cancel(0);observed_auth_epoch=epoch;}
        if(face_found<=0){bio_worker_cancel(CREDENTIAL_FACE);face_quality_ms=0;}
    }
    access_frame(ir,w,h,hand,hand_found,rgb_preview,ms);
    face_frame(ir,nv12,w,h,pose,face_found,hand_found,rgb_preview,ms);
    if(async_enabled) {
        if(access_consume_results(ms)) {
            if(hand_found==0&&face_found>0&&face&&work.face_ready&&work.ready)
                face_schedule(ir,nv12,w,h,pose,rgb_preview,ms);
            if(hand_found>0)palm_schedule(ir,w,h,&raw_hand,rgb_preview,ms);
        }
        if(hand_found>0)hand->status=palm_quality_ms&&ms-palm_quality_ms<1000?palm_quality_status:-1;
        if(face_found>0&&hand_found==0&&face_quality_ms&&ms-face_quality_ms<1000) {
            pose->status=face_quality_status;pose->sharpness=face_quality_sharpness;
        }
    }
}

void access_observe_finger(int found,uint64_t id,uint64_t ms)
{
    if(!finger)return;
    if(!id||found<0) {
        finger_enroll_observe(-1,ms);
        observed_finger=-1;finger_observed_ms=0;finger_missing=finger_candidates=0;
    } else {
        if(id==finger_observed_id)return; /* A copied/stalled frame is not another observation. */
        finger_observed_id=id;finger_observed_ms=ms;observed_finger=found;
        finger_enroll_observe(found,ms);
        if(finger_enrolling()&&work.finger_enroll_phase==FINGER_COLLECT) {
            int quality=found==2?STATUS_FINGERPRINT_AREA_TOO_SMALL:found==1?STATUS_OK:STATUS_FAILED;
            if(found>0&&work.status!=quality) {
                work.status=quality;
                snprintf(work.message,sizeof(work.message),"%s",found==2?"接触面积不足，请完整按压手指":"正在采集，请保持同一手指不动");publish();
            }
        }
        if(found>0)finger_missing=0;
        else {
            finger_candidates=0;finger_unmatched=0;
            if(finger_missing<FINGER_LEAVE_FRAMES&&++finger_missing==FINGER_LEAVE_FRAMES) {
                finger_held=finger_latched=0;publish();
                printf("FINGER_SESSION left false_frames=%d\n",finger_missing);
            }
        }
    }
    if(async_enabled) {
        if(observed_finger!=1)bio_worker_cancel(CREDENTIAL_FINGER);
        if(finger_blocks()){bio_worker_cancel(CREDENTIAL_PALM);bio_worker_cancel(CREDENTIAL_FACE);}
    }
    if(finger_blocks()){candidate_count=face_candidates=0;palm_quality_ms=face_quality_ms=0;}
}
static void finger_complete(BioJob *job,uint64_t ms)
{
    if(job->result!=STATUS_OK||job->finger.status!=STATUS_OK||!job->feature_size||job->feature_size>APP_FEATURE_BYTES) {
        finger_candidates=0;
        if(finger_enrolling()) {
            work.status=job->result;snprintf(work.message,sizeof(work.message),"%s",job->result==STATUS_FINGERPRINT_AREA_TOO_SMALL?"接触面积不足，请完整按压手指":"指纹采集失败，请调整手指");publish();
        }
        return;
    }
    if(finger_enrolling()) {
        if(work.finger_enroll_phase!=FINGER_COLLECT||finger_press_count>=FINGER_ENROLL_PER_PRESS)return;
        memcpy(finger_press_features[finger_press_count],job->feature,job->feature_size);
        finger_press_sizes[finger_press_count++]=job->feature_size;
        work.finger_enroll_samples=finger_press_count;
        work.progress=finger_presses_done*FINGER_ENROLL_PER_PRESS+finger_press_count;
        work.total=FINGER_ENROLL_TOTAL;
        if(finger_press_count<FINGER_ENROLL_PER_PRESS) {publish();return;}
        User_For_C_t user={.uuid=enrolling_template_uuid,.isAdmin=enrolling.is_admin};
        app_utf8_copy(user.userName,sizeof(user.userName),enrolling.name);
        for(int i=0;i<FINGER_ENROLL_PER_PRESS;i++) {
            if(finger->enroll(&user,finger_press_features[i],finger_press_sizes[i],&progress))progress.status=STATUS_REGIS_FAILED;
            int expected=finger_presses_done*FINGER_ENROLL_PER_PRESS+i+1;
            if((progress.status==STATUS_IN_REGISING||progress.status==STATUS_REGIS_SUCCESS)&&
               (progress.total!=FINGER_ENROLL_TOTAL||progress.progress!=expected||
                (progress.status==STATUS_REGIS_SUCCESS)!=(expected==FINGER_ENROLL_TOTAL)))progress.status=STATUS_REGIS_FAILED;
            if(progress.status!=STATUS_IN_REGISING&&progress.status!=STATUS_REGIS_SUCCESS)break;
        }
        finger_press_clear();
        work.status=progress.status;work.progress=progress.progress;work.total=progress.total;
        if(progress.status==STATUS_REGIS_SUCCESS) {
            uint16_t id=enrolling.local_id;work.registering=0;finger->clear();
            printf("FINGER_ENROLL success local_id=%u fingerprint_uuid=%u progress=%d/%d presses=5\n",id,enrolling_template_uuid,progress.progress,progress.total);
            int restored=finger_enroll_restore();
            if(finger->complete(enrolling_template_uuid)!=1||store_finish_enroll_kind(&store,id,-1,CREDENTIAL_FINGER)) {
                work.finger_ready=0;event(ACCESS_EVENT_ERROR,id,"指纹注册保存待恢复，请重启重试");
            } else event(restored?ACCESS_EVENT_ERROR:ACCESS_EVENT_ENROLLED,id,restored?"指纹已保存，参数恢复失败，请重启":"指纹注册成功");
        } else if(progress.status==STATUS_REGIS_FAILED||progress.status==STATUS_REGIS_FAILED_ALREADY_REGIS||progress.status==STATUS_REGIS_FAILED_MULTIY_REGIS) {
            int status=progress.status;cancel("fingerprint enrollment rejected");
            event(ACCESS_EVENT_ERROR,enrolling.local_id,status==STATUS_REGIS_FAILED_ALREADY_REGIS?"该指纹已注册，请勿重复注册":"指纹注册失败，请重试");
        } else if(progress.status==STATUS_IN_REGISING) {
            ++finger_presses_done;work.finger_enroll_phase=FINGER_WAIT_RELEASE;
            finger_empty_since=finger_empty_last=0;
            snprintf(work.message,sizeof(work.message),"本轮10帧已采满，请抬起手指并等待2秒");
            printf("FINGER_ENROLL press_complete=%d progress=%d/50\n",finger_presses_done,work.progress);publish();
        } else {cancel("fingerprint unexpected status");event(ACCESS_EVENT_ERROR,enrolling.local_id,"指纹注册失败，请重试");}
        return;
    }
    pthread_mutex_lock(&lock);int enabled=auth_enabled;pthread_mutex_unlock(&lock);
    if(!enabled||work.registering||finger_held||finger_latched||!job->auth_ran)return;
    AppUser u;
    int matched=job->auth_result>0&&!store_get_by_template(&store,CREDENTIAL_FINGER,job->user.uuid,&u)&&
        u.enabled&&!u.deleting&&u.finger_state==CRED_REGISTERED;
    if(matched) {
        finger_unmatched=0;
        if(!finger_candidates||finger_candidate!=job->user.uuid){finger_candidate=job->user.uuid;finger_candidates=1;}else ++finger_candidates;
        if(finger_candidates>=work.app_parameters.match_confirmations&&!success_cooldown_remaining(ms)) {
            uint64_t begin=access_us();int rc=store_record_match_kind(&store,&u,job->user.targetDist,ms,CREDENTIAL_FINGER);
            trace_stage("fingerprint","attendance",begin,rc);
            if(rc)snprintf(work.recognition,sizeof(work.recognition),"识别成功，记录保存失败");
            else snprintf(work.recognition,sizeof(work.recognition),"指纹识别成功  %s\n工号 %s",*u.name?u.name:"未设置姓名",u.employee_id);
            work.matched=1;work.score=job->user.targetDist;last_event=ms;finger_held=finger_latched=1;
            recognized(CREDENTIAL_FINGER,ms);
            printf("ACCESS fingerprint match local_id=%u finger_uuid=%u score=%.4f samples=%d\n",u.local_id,job->user.uuid,job->user.targetDist,finger_candidates);
        }
    } else {
        finger_candidates=0;if(!finger_unmatched)finger_unmatched=ms;
        if(ms-finger_unmatched>2000&&ms-last_event>=3000) {
            store_record_unknown_kind(&store,ms,CREDENTIAL_FINGER);
            snprintf(work.recognition,sizeof(work.recognition),"未识别到已注册用户");work.matched=-1;last_event=ms;finger_latched=1;publish();
        }
    }
}
void access_fingerprint_frame(const uint8_t *gray,uint16_t w,uint16_t h,uint64_t id,int dvp_preview,uint64_t ms)
{
    (void)dvp_preview; /* Registration uses an opaque UI, independent of preview source. */
    if(!finger||!work.ready||!work.finger_ready||observed_finger!=1||!id||id!=finger_observed_id||id<=finger_submitted_id)return;
    if(work.registering&&(!finger_enrolling()||work.finger_enroll_phase!=FINGER_COLLECT))return;
    pthread_mutex_lock(&lock);int enabled=auth_enabled;pthread_mutex_unlock(&lock);
    if(!work.registering&&(!enabled||finger_held||finger_latched))return;
    BioJob job={.kind=CREDENTIAL_FINGER,.stage=work.registering?BIO_FEATURE:BIO_FINGER_RECOGNIZE,.sample_id=id,.submitted_ms=ms};
    if(async_enabled) {
        if(!async_quiescing&&bio_worker_submit(&job,gray,w,h,NULL)>0)finger_submitted_id=id;
    } else {
        finger_submitted_id=id;
        job.result=finger->features(gray,w,h,id,&job.finger,job.feature,sizeof(job.feature),&job.feature_size);
        if(!work.registering&&job.result==STATUS_OK) {job.auth_ran=1;job.auth_result=finger->recognize(job.feature,job.feature_size,&job.user);}
        finger_complete(&job,ms);memset(&job,0,sizeof(job));
    }
}
