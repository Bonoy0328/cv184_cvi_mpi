#define main previous_biometrics_suite
#include "biometric_test.c"
#undef main
#include "biometric_worker.h"
#include "fingerprint_image.h"
#include <stdatomic.h>
static uint8_t fingers[100],gray[FINGER_BYTES];
static int fp_features,fp_auth,fp_progress,fp_picks,fp_enrolling;
static int fp_capacity(void){return 50;}
static int fp_area(int active){fp_enrolling=active;return 0;}
static uint64_t seq=1,last_enroll_id;
static atomic_int block_fp,entered_fp,block_palm,entered_palm;
static int blocking_palm_features(uint8_t *y,uint16_t w,uint16_t h,HandPoseRes_t *p,uint8_t *feat)
{
    atomic_store(&entered_palm,1);
    while(atomic_load(&block_palm))usleep(1000);
    return h_features(y,w,h,p,feat);
}
static int fp_ids(uint16_t *ids,int cap){int n=0;for(int i=0;i<100;i++)if(fingers[i]){assert(n<cap);ids[n++]=i;}return n;}
static int fp_exists(uint16_t id){return fingers[id];}
static int fp_delete(uint16_t id){fingers[id]=0;return 0;}
static int fp_clear(void){fp_progress=0;last_enroll_id=0;return 0;}
static int fp_free(void){++fp_picks;for(int i=0;i<100;i++)if(!fingers[i])return i;return -1;}
static int fp_detect(const uint8_t *g,uint16_t w,uint16_t h,uint64_t id,FingerprintRes_t *r){assert(w==480&&h==640);r->sample_id=id;r->status=g[0]?STATUS_OK:STATUS_FINGERPRINT_NO_FINGER;return r->status;}
static int fp_feat(const uint8_t *g,uint16_t w,uint16_t h,uint64_t id,FingerprintRes_t *r,uint8_t *out,uint32_t cap,uint32_t *size){
    assert(g[0]==42&&w==480&&h==640&&cap>=156);++fp_features;atomic_store(&entered_fp,1);
    while(atomic_load(&block_fp))usleep(1000);
    assert(g[0]==42);memset(out,0,156);memcpy(out,&id,sizeof(id));*size=156;r->status=STATUS_OK;r->sample_id=id;return STATUS_OK;
}
static int fp_enroll(User_For_C_t *u,const uint8_t *f,uint32_t size,ResgisRes_t *p){
    assert(size==156);uint64_t id;memcpy(&id,f,sizeof(id));assert(id>last_enroll_id);last_enroll_id=id;
    p->progress=++fp_progress;p->total=50;p->status=fp_progress==50?STATUS_REGIS_SUCCESS:STATUS_IN_REGISING;
    if(p->status==STATUS_REGIS_SUCCESS)fingers[u->uuid]=1;
    return 0;
}
static int fp_recognize(const uint8_t *f,uint32_t size,User_For_C_t *u){(void)f;assert(size==156);++fp_auth;u->uuid=0;u->targetDist=.94f;return fingers[0];}
static const FingerBackend fp={fp_ids,fp_exists,fp_exists,fp_delete,fp_clear,fp_capacity,fp_free,fp_detect,fp_feat,fp_enroll,fp_recognize,fp_area};
static void observe(int found){ms+=30;access_observe_finger(found,++seq,ms);}
static void finger_frame(int found){access_poll(ms);observe(found);access_fingerprint_frame(gray,480,640,seq,0,ms);access_snapshot(&snap,0);}
static void rotation(void){
    uint8_t src[6*3],out[12];memset(src,255,sizeof(src));
    for(int y=0;y<3;y++)for(int x=0;x<2;x++)src[y*6+x*2]=y*2+x;
    memset(out,0xEF,sizeof(out));assert(!fingerprint_rotate_yuyv(src,sizeof(src),2,3,6,out,6));
    const uint8_t expected[]={1,3,5,0,2,4};assert(!memcmp(out,expected,6));for(int i=6;i<12;i++)assert(out[i]==0xEF);
    assert(fingerprint_rotate_yuyv(src,17,2,3,6,out,6)<0);assert(fingerprint_rotate_yuyv(src,18,2,3,3,out,6)<0);
    uint8_t *full=malloc(1288*480),*dst=malloc(FINGER_BYTES);assert(full&&dst);
    for(int y=0;y<480;y++)for(int x=0;x<640;x++)full[y*1288+2*x]=(y*7+x*3)%251;
    assert(!fingerprint_rotate_yuyv(full,1288*480,640,480,1288,dst,FINGER_BYTES));
    for(int y=0;y<480;y++)for(int x=0;x<640;x++)assert(dst[(639-x)*480+y]==(y*7+x*3)%251);
    free(full);free(dst);puts("PASS DVP native YUYV stride, full 640x480 CCW90 and output guards");
}
int main(void){
    rotation();char dir[]="/tmp/ky50-finger-test-XXXXXX",path[256];assert(mkdtemp(dir));
    ir[0]=37;nv12[0]=89;nv12[540*960]=51;nv12[540*960+1]=203;gray[0]=42;
    assert(!access_open_biometrics(dir,&palm,&face));uint16_t person=create_user("5000");enroll_face(person);enroll_palm(person);
    access_close();snprintf(path,sizeof(path),"%s/users.sqlite3",dir);sqlite3 *old;assert(!sqlite3_open(path,&old));
    assert(!sqlite3_exec(old,"PRAGMA user_version=3; UPDATE credentials SET state=2,template_uuid=7,model_id='old-full-image' WHERE kind='fingerprint'",NULL,NULL,NULL));sqlite3_close(old);fingers[7]=1;
    assert(!access_open_modalities(dir,&palm,&face,&fp));UserStore view;assert(!store_open(&view,path));
    AppUser u=get_user(&view,person);assert(!fingers[7]&&u.finger_uuid<0);assert(u.face_uuid==0&&u.palm_uuid==0&&u.registered==3&&scalar(view.db,"PRAGMA user_version")==4);
    uint16_t finger_person=create_user("5001");command((AccessCommand){.type=ACCESS_ENROLL,.local_id=finger_person,.modality=CREDENTIAL_FINGER});assert(snap.registering&&fp_picks==1);
    for(int i=0;i<5;i++){finger_frame(0);}assert(fp_features==0&&fp_progress==0);
    assert(fp_enrolling==1 && snap.total==50);
    // An early lift abandons only the current incomplete press.
    for(int i=0;i<4;i++)finger_frame(1);
    assert(snap.progress==4 && fp_progress==0);
    int area_before=fp_features;
    for(int i=0;i<75;i++)finger_frame(2);
    assert(fp_features==area_before&&snap.progress==4&&snap.status==STATUS_FINGERPRINT_AREA_TOO_SMALL);
    assert(snap.finger_enroll_phase==FINGER_COLLECT);
    finger_frame(0);assert(snap.progress==0 && fp_progress==0);
    for(int i=0;i<70;i++)finger_frame(0);
    for(int press=0;press<5;press++) {
        for(int i=0;i<10;i++) {
            finger_frame(1);int n=fp_features;
            access_fingerprint_frame(gray,480,640,seq,0,ms);assert(fp_features==n);
            assert(snap.progress==press*10+i+1);
        }
        if(press==4)break;
        assert(snap.registering && snap.finger_enroll_phase==FINGER_WAIT_RELEASE);
        int n=fp_features;
        for(int i=0;i<75;i++)finger_frame(2);
        assert(fp_features==n && snap.finger_enroll_phase==FINGER_WAIT_RELEASE);
        for(int i=0;i<20;i++)finger_frame(1);
        assert(fp_features==n && snap.progress==(press+1)*10);
        // A repeated cached empty frame cannot satisfy a two-second absence.
        finger_frame(0);ms+=2200;access_observe_finger(0,seq,ms);
        finger_frame(1);assert(fp_features==n);
        // Short absence, re-press and device errors each interrupt the timer.
        for(int i=0;i<40;i++)finger_frame(0);
        finger_frame(1);assert(fp_features==n);
        for(int i=0;i<40;i++)finger_frame(0);
        finger_frame(-1);
        for(int i=0;i<40;i++)finger_frame(0);
        finger_frame(1);assert(fp_features==n);
        for(int i=0;i<70;i++)finger_frame(0);
        assert(snap.finger_enroll_phase==FINGER_WAIT_PRESS);
    }
    assert(fp_enrolling==0);
    assert(!snap.registering&&snap.event==ACCESS_EVENT_ENROLLED&&fp_picks==1);
    u=get_user(&view,finger_person);assert(u.finger_uuid==0&&u.face_uuid==-1&&u.palm_uuid==-1&&u.registered==4);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,1,0,0}});access_enable_recognition(1);
    int hb=hand_feats,fb=face_feats;finger_frame(1);frame(1,1);frame(1,1);
    assert(snap.success_sequence==1&&snap.success_modality==CREDENTIAL_FINGER&&hand_feats==hb&&face_feats==fb);
    int before=fp_auth;for(int i=0;i<8;i++)finger_frame(1);assert(fp_auth==before);
    finger_frame(0);
    for(int i=0;i<8;i++){ms+=10;access_observe_finger(0,seq,ms);}
    finger_frame(1);assert(fp_auth==before); /* Cached empty frame cannot release a held finger. */
    for(int i=0;i<4;i++){finger_frame(0);}finger_frame(-1);finger_frame(1);assert(fp_auth==before);
    for(int i=0;i<5;i++){finger_frame(0);}finger_frame(1);assert(fp_auth==before+1&&snap.success_sequence==2);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,2,0,0}});
    for(int i=0;i<5;i++){finger_frame(0);}finger_frame(1);assert(snap.success_sequence==2);
    for(int i=0;i<4;i++){ms+=10;access_observe_finger(1,seq,ms);access_fingerprint_frame(gray,480,640,seq,1,ms);}
    access_snapshot(&snap,0);assert(snap.success_sequence==2); /* Cached match cannot confirm twice. */
    finger_frame(1);assert(snap.success_sequence==3);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,1,0,0}});
    for(int i=0;i<5;i++){finger_frame(0);}frame(1,1);assert(snap.success_modality==CREDENTIAL_PALM);
    for(int i=0;i<3;i++){frame(0,0);}frame(0,1);assert(snap.success_modality==CREDENTIAL_FACE);
    /* A small contact is not a lift, but cannot monopolize authentication.
     * In particular it must not starve palm/face after a successful finger. */
    finger_frame(1);before=fp_auth;unsigned successes=snap.success_sequence;
    for(int i=0;i<3;i++){frame(0,0);}
    for(int i=0;i<3;i++){finger_frame(2);frame(1,1);}
    assert(snap.success_sequence==successes+1&&snap.success_modality==CREDENTIAL_PALM);
    assert(fp_auth==before);
    for(int i=0;i<3;i++){finger_frame(2);frame(0,0);}
    finger_frame(2);frame(0,1);
    assert(snap.success_sequence==successes+2&&snap.success_modality==CREDENTIAL_FACE);
    finger_frame(1);assert(fp_auth==before); /* 108 never unlocks the same finger. */
    // Cancel and timeout both restore 0.15 and remove pending SDK/UI samples.
    command((AccessCommand){.type=ACCESS_ENROLL,.local_id=person,.modality=CREDENTIAL_FINGER});
    assert(fp_enrolling && snap.registering);finger_frame(1);
    command((AccessCommand){.type=ACCESS_CANCEL});assert(!fp_enrolling && !snap.registering && fp_progress==0);
    command((AccessCommand){.type=ACCESS_ENROLL,.local_id=person,.modality=CREDENTIAL_FINGER});
    assert(fp_enrolling);ms+=120001;access_poll(ms);access_snapshot(&snap,0);
    assert(!fp_enrolling && !snap.registering && fp_progress==0);
    u=get_user(&view,person);assert(u.face_uuid==0&&u.palm_uuid==0&&u.finger_uuid<0);
    store_close(&view);access_close();
    /* Same owned-image worker as palm/face; absent/error cancels in-flight auth. */
    PalmBackend async_palm=palm;async_palm.features=blocking_palm_features;
    assert(!access_open_modalities(dir,&async_palm,NULL,&fp));assert(!access_start_async());access_enable_recognition(1);
    atomic_store(&block_fp,1);atomic_store(&entered_fp,0);finger_frame(1);
    for(int i=0;i<1000&&!atomic_load(&entered_fp);i++){usleep(1000);}assert(atomic_load(&entered_fp));
    gray[0]=99;before=fp_auth;observe(2);atomic_store(&block_fp,0);
    for(int i=0;i<1000&&bio_worker_busy();i++){usleep(1000);}assert(!bio_worker_busy()&&fp_auth==before);gray[0]=42;
    finger_frame(1);assert(bio_worker_wait_result(1000));assert(access_consume_results(ms+1));access_snapshot(&snap,0);
    assert(snap.success_sequence==1&&snap.success_modality==CREDENTIAL_FINGER);
    /* 108 must allow scheduling AND keep an in-flight palm result valid. */
    observe(2);atomic_store(&block_palm,1);atomic_store(&entered_palm,0);
    frame(1,0);frame(1,0);
    for(int i=0;i<1000&&!atomic_load(&entered_palm);i++){usleep(1000);}assert(atomic_load(&entered_palm));
    observe(2);atomic_store(&block_palm,0);
    assert(bio_worker_wait_result(1000));assert(access_consume_results(ms+1));access_snapshot(&snap,0);
    assert(snap.success_sequence==2&&snap.success_modality==CREDENTIAL_PALM);
    /* A qualified finger still preempts a pending palm authentication. */
    for(int i=0;i<3;i++){frame(0,0);}
    atomic_store(&block_palm,1);atomic_store(&entered_palm,0);frame(1,0);frame(1,0);
    for(int i=0;i<1000&&!atomic_load(&entered_palm);i++){usleep(1000);}assert(atomic_load(&entered_palm));
    hb=hand_auth;observe(1);atomic_store(&block_palm,0);
    for(int i=0;i<1000&&bio_worker_busy();i++){usleep(1000);}assert(!bio_worker_busy()&&hand_auth==hb);
    assert(!access_consume_results(ms+1));access_snapshot(&snap,0);assert(snap.success_sequence==2);
    command((AccessCommand){.type=ACCESS_DELETE,.count=1,.ids={finger_person}});assert(!fingers[0]&&faces[0]&&hands[0]);
    access_close();clean_dir(dir);puts("PASS fingerprint: schema3 migration, independent SDK ID0, registration, no-finger skip, three-way priority, departure/error, async copy/cancel, relay event and deletion");return 0;
}
