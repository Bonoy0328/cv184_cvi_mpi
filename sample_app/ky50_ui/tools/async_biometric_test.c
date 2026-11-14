/* Reuse the deterministic stores/backends; this suite exercises the real worker. */
#define main synchronous_suite_main
#include "biometric_test.c"
#undef main
#include "biometric_worker.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
static atomic_int blocked_stage,entered_stage,palm_completed,ir_completed;
static int ae_target=56,ae_sets,ae_stable=1;
static uint64_t capture_sequence;
static int ae_get(IrExposureReading *r)
{
    *r=(IrExposureReading){.target=ae_target,.stable=ae_stable,.sequence=capture_sequence};return 0;
}
static int ae_set(int target){ae_target=target;++ae_sets;return 0;}
static const IrExposureBackend ae_backend={ae_get,ae_set};
static void gate(int stage)
{
    atomic_store(&entered_stage,stage);
    while(atomic_load(&blocked_stage)==stage)usleep(1000);
}
static void wait_gate(int stage)
{
    for(int i=0;i<2000&&atomic_load(&entered_stage)!=stage;i++)usleep(1000);
    assert(atomic_load(&entered_stage)==stage);
}
static void drain(void)
{
    for(int i=0;i<2000&&bio_worker_busy();i++)usleep(1000);
    assert(!bio_worker_busy());
}
typedef struct { uint8_t image[540*960*3/2];FacePoseRes_t pose; } Sample;
static void *sample_capture(const uint8_t *image,uint16_t w,uint16_t h,const FacePoseRes_t *p)
{
    assert(w==540&&h==960);Sample *s=malloc(sizeof(*s));assert(s);
    memcpy(s->image,image,sizeof(s->image));s->pose=*p;return s;
}
static int sample_features(const void *ptr,FacePoseRes_t *p,uint8_t *feat)
{
    const Sample *s=ptr;gate(1);*p=s->pose;
    return f_features((uint8_t *)s->image,540,960,p,feat);
}
static void sample_free(void *ptr){free(ptr);}
static int slow_auth(uint8_t *feat,User_For_C_t *u){gate(2);return f_auth(feat,u);}
static int slow_palm_features(uint8_t *image,uint16_t w,uint16_t h,HandPoseRes_t *p,uint8_t *feat)
{
    gate(3);int rc=h_features(image,w,h,p,feat);atomic_fetch_add(&palm_completed,1);return rc;
}
static int slow_palm_auth(uint8_t *feat,User_For_C_t *u){gate(4);return h_auth(feat,u);}
static int slow_ir_live(uint8_t *image,uint16_t w,uint16_t h)
{
    gate(5);int rc=f_live_ir(image,w,h);atomic_store(&ir_completed,1);return rc;
}
static void tick(int hand,int found)
{
    HandPoseRes_t hp={0};FacePoseRes_t fp={0};ms+=50;
    IrExposureSample sample={.sequence=++capture_sequence,.captured_ms=ms,.target=ae_target,.stable=ae_stable,.valid=1};
    access_set_ir_sample(&sample);
    access_poll(ms);access_biometrics_frame(ir,nv12,540,960,&hp,hand,&fp,found,1,ms);
    access_snapshot(&snap,0);usleep(1000);
}
static int events(UserStore *db){return scalar(db->db,"SELECT count(*) FROM attendance_events");}
int main(void)
{
    char dir[]="/tmp/ky50-async-test-XXXXXX",path[256];assert(mkdtemp(dir));
    ir[0]=37;nv12[0]=89;nv12[540*960]=51;nv12[540*960+1]=203;
    PalmBackend async_palm=palm;async_palm.features=slow_palm_features;async_palm.recognize=slow_palm_auth;
    FaceBackend async_face=face;
    async_face.sample_capture=sample_capture;async_face.sample_features=sample_features;
    async_face.sample_free=sample_free;async_face.recognize=slow_auth;async_face.live_ir=slow_ir_live;
    assert(!access_open_biometrics(dir,&async_palm,&async_face));
    uint16_t id=create_user("12001");enroll_face(id);enroll_palm(id);
    access_set_exposure_backend(&ae_backend);
    command((AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={5,5,5,2,0,0}});
    snprintf(path,sizeof(path),"%s/users.sqlite3",dir);UserStore view;assert(!store_open(&view,path));
    access_enable_recognition(1);assert(!access_start_async());
    for(int i=0;i<5;i++)tick(0,0);
    int before=face_auth;atomic_store(&blocked_stage,1);atomic_store(&entered_stage,0);
    tick(0,1);wait_gate(1);
    /* Camera memory can be reused as soon as submission returns. */
    nv12[0]=42;
    for(int i=0;i<4;i++)tick(0,1);
    assert(bio_worker_busy()&&events(&view)==0);
    /* Raw hand entry cancels face immediately, even before the palm enter count. */
    tick(1,1);atomic_store(&blocked_stage,0);drain();nv12[0]=89;
    assert(face_auth==before&&events(&view)==0);
    for(int i=0;i<5;i++)tick(0,0);
    /* A source epoch/gap invalidates a task even if the new scene has a face. */
    atomic_store(&blocked_stage,1);atomic_store(&entered_stage,0);
    tick(0,1);wait_gate(1);access_detection_gap();
    atomic_store(&blocked_stage,0);drain();assert(face_auth==before);
    /* A fast leave/re-enter between poll and completed detection is also a new page epoch. */
    atomic_store(&blocked_stage,1);atomic_store(&entered_stage,0);
    for(int i=0;i<10&&atomic_load(&entered_stage)!=1;i++)tick(0,1);
    wait_gate(1);access_enable_recognition(0);access_enable_recognition(1);
    {HandPoseRes_t hp={0};FacePoseRes_t fp={0};ms+=50;
     access_biometrics_frame(ir,nv12,540,960,&hp,0,&fp,1,1,ms);}
    atomic_store(&blocked_stage,0);drain();assert(face_auth==before);
    /* Complete two background feature/auth pairs and latch one event. */
    for(int i=0;i<300&&events(&view)==0;i++)tick(0,1);
    assert(events(&view)==1&&snap.matched==1);
    before=face_auth;
    for(int i=0;i<25;i++)tick(0,1);
    assert(face_auth==before&&events(&view)==1);
    /* In-flight auth cannot commit after five raw misses. */
    for(int i=0;i<5;i++)tick(0,0);
    atomic_store(&blocked_stage,2);atomic_store(&entered_stage,0);
    for(int i=0;i<80&&atomic_load(&entered_stage)!=2;i++)tick(0,1);
    wait_gate(2);
    for(int i=0;i<5;i++)tick(0,0);
    atomic_store(&blocked_stage,0);drain();assert(events(&view)==1);
    /* A live configuration command drains old jobs without blocking detections. */
    atomic_store(&blocked_stage,1);atomic_store(&entered_stage,0);
    for(int i=0;i<20&&atomic_load(&entered_stage)!=1;i++)tick(0,1);
    wait_gate(1);
    AccessCommand c={.type=ACCESS_APP_PARAMETERS,.app_parameters={3,7,5,2,0,0}};
    assert(!access_submit(&c));tick(0,1);
    assert(snap.app_parameters.face_leave_frames==5);
    atomic_store(&blocked_stage,0);drain();tick(0,1);
    assert(snap.app_parameters.face_leave_frames==7&&snap.app_parameters.palm_leave_frames==3);
    for(int i=0;i<8;i++)tick(0,0);
    before=face_auth;int face_features_before=face_feats;
    atomic_store(&blocked_stage,3);atomic_store(&entered_stage,0);atomic_store(&palm_completed,0);
    tick(1,1);tick(1,1);wait_gate(3);ir[0]=42;
    for(int i=0;i<4;i++)tick(1,1);
    assert(bio_worker_busy()&&events(&view)==1);
    atomic_store(&blocked_stage,0);
    for(int i=0;i<2000&&!atomic_load(&palm_completed);i++)usleep(1000);
    assert(atomic_load(&palm_completed));ir[0]=37;
    for(int i=0;i<300&&events(&view)<2;i++)tick(1,1);
    assert(events(&view)==2&&face_auth==before&&face_feats==face_features_before);
    before=hand_auth;
    for(int i=0;i<25;i++)tick(1,1);
    assert(hand_auth==before&&events(&view)==2);
    for(int i=0;i<3;i++)tick(0,0);
    atomic_store(&blocked_stage,4);atomic_store(&entered_stage,0);
    for(int i=0;i<80&&atomic_load(&entered_stage)!=4;i++)tick(1,1);
    wait_gate(4);
    for(int i=0;i<3;i++)tick(0,0);
    atomic_store(&blocked_stage,0);drain();assert(events(&view)==2);
    /* The previous async implementation silently failed every success save:
     * auth was collected at a different ms than feature feedback. */
    int saved=-1;assert(!store_ir_target_load(&view,&saved)&&saved==56);
    hand_quality=STATUS_TOO_BRIGHT;
    for(int i=0;i<80&&ae_target==56;i++)tick(1,1);
    assert(ae_target==52&&ae_sets==1);hand_quality=STATUS_OK;ae_stable=0;
    int prior_auth=hand_auth;
    for(int i=0;i<300&&events(&view)<3;i++)tick(1,1);
    assert(events(&view)==3&&hand_auth>=prior_auth+2);
    assert(!store_ir_target_load(&view,&saved)&&saved==52);
    ae_stable=1;
    for(int i=0;i<3;i++)tick(0,0);
    ae_target=70;hand_quality=STATUS_TOO_BRIGHT;int prior_sets=ae_sets;
    for(int i=0;i<80&&ae_sets==prior_sets;i++)tick(1,1);
    assert(ae_sets==prior_sets+1&&ae_target==52); /* recalls latest success */
    hand_quality=STATUS_OK;for(int i=0;i<8;i++)tick(0,0);
    c=(AccessCommand){.type=ACCESS_APP_PARAMETERS,.app_parameters={3,3,1,1,0,1}};
    assert(!access_submit(&c));tick(0,0);assert(snap.app_parameters.strong_liveness);
    FaceParameters rgb_off=face_settings;rgb_off.liveness_enable=0;
    AccessCommand rgb_command={.type=ACCESS_FACE_PARAMETERS,.face_parameters=rgb_off};
    assert(!access_submit(&rgb_command));tick(0,0);
    assert(!snap.face_parameters.liveness_enable&&snap.app_parameters.strong_liveness);
    rgb_live_fail=1; /* Must not block IR-only jobs. */
    int old_events=events(&view);ir_live_pass=0;
    for(int i=0;i<20;i++)tick(0,1);
    assert(events(&view)==old_events);
    ir_live_pass=1;atomic_store(&blocked_stage,5);atomic_store(&entered_stage,0);atomic_store(&ir_completed,0);
    for(int i=0;i<80&&atomic_load(&entered_stage)!=5;i++)tick(0,1);
    wait_gate(5);ir[0]=42;for(int i=0;i<3;i++)tick(0,1);
    atomic_store(&blocked_stage,0);
    for(int i=0;i<2000&&!atomic_load(&ir_completed);i++)usleep(1000);
    assert(atomic_load(&ir_completed));ir[0]=37;
    for(int i=0;i<80&&events(&view)==old_events;i++)tick(0,1);
    assert(events(&view)==old_events+1);old_events=events(&view);
    for(int i=0;i<3;i++)tick(0,0);
    atomic_store(&blocked_stage,5);atomic_store(&entered_stage,0);
    for(int i=0;i<80&&atomic_load(&entered_stage)!=5;i++)tick(0,1);
    wait_gate(5);for(int i=0;i<3;i++)tick(0,0);
    atomic_store(&blocked_stage,0);drain();assert(events(&view)==old_events);
    atomic_store(&blocked_stage,5);atomic_store(&entered_stage,0);
    for(int i=0;i<80&&atomic_load(&entered_stage)!=5;i++)tick(0,1);
    wait_gate(5);c.app_parameters.strong_liveness=0;assert(!access_submit(&c));tick(0,1);
    assert(snap.app_parameters.strong_liveness); /* wait for cancelled SDK call before changing policy */
    atomic_store(&blocked_stage,0);drain();tick(0,1);
    assert(!snap.app_parameters.strong_liveness);
    for(int i=0;i<80&&events(&view)==old_events;i++)tick(0,1);
    assert(events(&view)==old_events+1);
    rgb_live_fail=0;
    puts("PASS async IR: independent owned NV12, false blocks event, departure cancels IR result, live toggle drains old SDK call");
    /* Completion must publish without another detector round. */
    for(int i=0;i<3;i++)tick(0,0);
    old_events=events(&view);atomic_store(&blocked_stage,1);atomic_store(&entered_stage,0);
    for(int i=0;i<80&&atomic_load(&entered_stage)!=1;i++)tick(0,1);
    wait_gate(1);atomic_store(&blocked_stage,0);
    assert(bio_worker_wait_result(2000));ms+=10;
    assert(access_consume_results(ms)&&events(&view)==old_events+1);
    assert(!access_consume_results(ms)&&events(&view)==old_events+1);
    access_snapshot(&snap,0);assert(snap.verified_modalities&CREDENTIAL_FACE);
    /* A palm observation arriving before RGB Pose cancels the face result. */
    for(int i=0;i<3;i++)tick(0,0);
    old_events=events(&view);atomic_store(&blocked_stage,1);atomic_store(&entered_stage,0);
    for(int i=0;i<80&&atomic_load(&entered_stage)!=1;i++)tick(0,1);
    wait_gate(1);atomic_store(&blocked_stage,0);assert(bio_worker_wait_result(2000));
    access_observe_hand(1,ms+10);assert(!access_consume_results(ms+10));
    assert(events(&view)==old_events);
    /* No fresh detection: a completed task cannot authorize stale presence. */
    for(int i=0;i<3;i++)tick(0,0);
    atomic_store(&blocked_stage,1);atomic_store(&entered_stage,0);
    for(int i=0;i<80&&atomic_load(&entered_stage)!=1;i++)tick(0,1);
    wait_gate(1);atomic_store(&blocked_stage,0);assert(bio_worker_wait_result(2000));
    ms+=300;assert(access_consume_results(ms));assert(events(&view)==old_events);
    /* Navigation cancels even if the detector has not polled its command queue. */
    atomic_store(&blocked_stage,1);atomic_store(&entered_stage,0);
    for(int i=0;i<80&&atomic_load(&entered_stage)!=1;i++)tick(0,1);
    wait_gate(1);atomic_store(&blocked_stage,0);assert(bio_worker_wait_result(2000));
    access_enable_recognition(0);access_enable_recognition(1);
    assert(!access_consume_results(ms+10)&&events(&view)==old_events);
    /* A pending configuration is an immediate cancellation boundary too. */
    atomic_store(&blocked_stage,1);atomic_store(&entered_stage,0);
    for(int i=0;i<80&&atomic_load(&entered_stage)!=1;i++)tick(0,1);
    wait_gate(1);atomic_store(&blocked_stage,0);assert(bio_worker_wait_result(2000));
    assert(!access_submit(&c));assert(!access_consume_results(ms+10)&&events(&view)==old_events);
    tick(0,0);assert(!bio_worker_wait_result(1));
    puts("PASS immediate result: no next frame needed, once per stay, raw palm priority, stale detection, navigation and queued config cancellation");
    /* Exit joins the worker before closing databases/backends. */
    access_close();store_close(&view);clean_dir(dir);
    puts("PASS async: owned input, blocked feature/auth, raw palm priority, departure cancellation, success hold, live config, async AE persistence and recall, clean shutdown");
    return 0;
}
