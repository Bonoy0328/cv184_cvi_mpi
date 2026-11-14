#include "media.h"
#include "ui.h"
#include "input.h"
#include "factory.h"
#include "relay_control.h"
#include "hand_overlay.h"
#include "access.h"
#include "biometric_worker.h"
#include "palm_queue.h"
#include <sys/resource.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t quit;
static volatile sig_atomic_t snapshot_requested,page_test_requested;
static atomic_int worker_stop;
static atomic_int source_request,source_current,source_busy,source_result;
static const char *settings_dir="/mnt/cfg/ky50-ui";
static const char *pva_config="pva/pvaConf.json";
static const char *user_data_dir="/mnt/system/ky50-data";
static int hand_enabled=1,frame_hand_state;
static atomic_int hand_ready,inference_stop,detection_gap;
static atomic_int result_stop;
static atomic_uint media_epoch;
static pthread_mutex_t algorithm_lock=PTHREAD_MUTEX_INITIALIZER;
static PalmQueue palm_queue;
static HandOverlayState overlay_state,face_overlay_state;
static int face_enabled;
static unsigned overlay_epoch;
static atomic_int hand_dump_requested;
static unsigned hand_frames,hand_hits,ir_failures;
static unsigned face_frames,face_hits,face_errors;
static uint64_t hand_time_us;

static pthread_mutex_t video_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char *latest;
static uint64_t video_ms;
static unsigned frames;
static int trace_stall;
static void signal_stop(int s) { (void)s;quit=1; }
static void signal_snapshot(int s) { (void)s;snapshot_requested=1; }
static void signal_page_test(int s) { (void)s;page_test_requested=1; }
/* Diagnostic only: real LVGL pointer callbacks, with video still running.
 * This does not replace the separate physical-finger acceptance test. */
static int page_test_step(uint64_t now)
{
    static unsigned step;
    static int active,down;
    static uint64_t next;
    static int x,y;
    if(page_test_requested) {
        page_test_requested=0;active=1;step=0;down=0;next=now+200;
        ui_show(UI_HOME,0);printf("PAGE_TEST start cycles=100 synthetic_pointer\n");
    }
    if(!active||now<next) return 0;
    unsigned phase=step%4,feature=(step/4)%8;
    if(!down) {
        x=phase==0?390:phase==1?110+(feature%2)*224:phase==2&&feature!=7&&feature!=0?240:52;
        y=phase==0?790:phase==1?230+(feature/2)*158:phase==2&&feature!=7&&feature!=0?522:100;
        ui_pointer(x,y,1);down=1;next=now+40;
    } else {
        ui_pointer(x,y,0);down=0;next=now+100;
        enum ui_page expected[]={UI_MENU,feature==7?UI_ADVANCED:feature==0?UI_USERS:UI_FEATURE,UI_MENU,UI_HOME};
        if(ui_current_page()!=expected[phase]) {
            printf("PAGE_TEST FAILED step=%u page=%d\n",step,ui_current_page());active=0;return -1;
        }
        if(++step==400) { active=0;printf("PAGE_TEST PASS cycles=100 transitions=400\n"); }
    }
    return 0;
}
/* Diagnostics retain the exact IR input / selected preview pair. Normal
 * preview runs independently and draws the most recent non-expired result. */
static void dump_hand_frame(const unsigned char *yuv,const unsigned char *rgb,
                            const HandPoseRes_t *pose,int found,enum camera_source preview,
                            const HandViewport *view)
{
    FILE *f=fopen("/tmp/ky50-ui/palm-ir.pgm","wb");
    if(f) { fprintf(f,"P5\n540 960\n255\n");fwrite(yuv,1,HAND_INPUT_WIDTH*HAND_INPUT_HEIGHT,f);fclose(f); }
    f=fopen("/tmp/ky50-ui/palm-overlay.ppm","wb");
    if(f) { fprintf(f,"P6\n480 854\n255\n");fwrite(rgb,3,UI_WIDTH*UI_HEIGHT,f);fclose(f); }
    f=fopen("/tmp/ky50-ui/palm-result.json","w");
    if(f) {
        fprintf(f,"{\"algorithm_source\":\"IR\",\"pipe\":0,\"preview\":\"%s\",\"width\":540,\"height\":960,\"found\":%d,\"status\":%d,\"rect\":[%u,%u,%u,%u],\"keypoints\":[",
            camera_source_name(preview),found,pose->status,pose->rect.centerX,pose->rect.centerY,
            pose->rect.centerWidth,pose->rect.centerHeight);
        for(int i=0;i<14;++i) fprintf(f,"%s%u",i?",":"",pose->keypoints[i]);
        fprintf(f,"],\"viewport\":{\"scaled_width\":%u,\"scaled_height\":%u,\"crop_x\":%d,\"crop_y\":%d}",
            view->scaled_width,view->scaled_height,view->crop_x,view->crop_y);
        if(found>0) {
            HandPoseRes_t copy=*pose;HandOverlay drawing;
            if(!hand_map(&copy,view,&drawing)) {
                fprintf(f,",\"screen_rect\":[%d,%d,%d,%d],\"screen_keypoints\":[",
                    drawing.left,drawing.top,drawing.right,drawing.bottom);
                for(int i=0;i<14;++i) fprintf(f,"%s%d",i?",":"",drawing.points[i]);
                fprintf(f,"]");
            }
        }
        fprintf(f,"}\n");fclose(f);
    }
    printf("PALM_DUMP source=IR pipe=0 preview=%s found=%d\n",camera_source_name(preview),found);
}
static uint64_t monotonic_us(void)
{
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}
typedef struct { uint64_t total,max;unsigned count; } Perf;
static Perf cap_perf[4],algorithm_perf[3],ui_perf[3];
static void perf_add(Perf *p,uint64_t us) { p->total+=us;if(us>p->max)p->max=us;++p->count; }
static void perf_log(const char *name,Perf *p) {
    printf("PERF %s n=%u avg_ms=%.2f max_ms=%.2f\n",name,p->count,p->count?p->total/(1000.0*p->count):0,p->max/1000.0);
    memset(p,0,sizeof(*p));
}
static void hide_overlay(void)
{
    pthread_mutex_lock(&video_lock);hand_overlay_reset(&overlay_state);hand_overlay_reset(&face_overlay_state);pthread_mutex_unlock(&video_lock);
}
static void *result_worker(void *unused)
{
    (void)unused;
    if(setpriority(PRIO_PROCESS,0,1))perror("BIO result consumer nice");
    while(!atomic_load(&result_stop)&&!atomic_load(&worker_stop)) {
        if(!bio_worker_wait_result(50))continue;
        pthread_mutex_lock(&algorithm_lock);
        if(!atomic_load(&result_stop)&&!atomic_load(&worker_stop)) {
            if(atomic_exchange(&detection_gap,0)){access_detection_gap();hide_overlay();}
            access_consume_results(ui_monotonic_ms());
        }
        pthread_mutex_unlock(&algorithm_lock);
    }
    return NULL;
}
static void *inference_worker(void *unused)
{
    (void)unused;PalmFrame frame;
    if(palm_frame_init(&frame)){atomic_store(&worker_stop,2);return NULL;}
    /* Linux nice is per-thread: UI and live capture keep normal priority. */
    if(setpriority(PRIO_PROCESS,0,3))perror("PALM worker nice");
    if(atomic_load(&hand_ready)&&access_start_async()){fprintf(stderr,"BIO worker start failed\n");palm_frame_free(&frame);atomic_store(&worker_stop,2);return NULL;}
    pthread_t results;int results_started=0;
    if(atomic_load(&hand_ready)) {
        if(pthread_create(&results,NULL,result_worker,NULL)){
            fprintf(stderr,"BIO result consumer start failed\n");palm_frame_free(&frame);atomic_store(&worker_stop,2);return NULL;
        }
        results_started=1;
    }
    uint64_t hand_log=ui_monotonic_ms()+5000;
    unsigned last_hand_frames=0,last_hand_hits=0;uint64_t last_hand_us=0;
    int previous_found=-99,previous_quality=-99,calibration_was_active=0;
    while(!atomic_load(&inference_stop)&&!atomic_load(&worker_stop)) {
        int have=palm_queue_take(&palm_queue,&frame);
        pthread_mutex_lock(&algorithm_lock);
        uint64_t now=ui_monotonic_ms();access_poll(now);
        if(atomic_exchange(&detection_gap,0)){access_detection_gap();hide_overlay();}
        if(have&&frame.epoch==atomic_load(&media_epoch)&&atomic_load(&hand_ready)) {
            if(now<frame.captured_ms||now-frame.captured_ms>250) {
                access_detection_gap();hide_overlay();
            } else {
                HandPoseRes_t pose={0};uint64_t begin=monotonic_us();
                /* Pose uses owned CPU images. Camera transitions may invalidate
                 * their epoch, but model teardown joins this thread first. */
                pthread_mutex_unlock(&algorithm_lock);
                int raw_found=frame.have_ir?handpose_detect(frame.yuv,HAND_INPUT_WIDTH,HAND_INPUT_HEIGHT,&pose):-1;
                uint64_t hand_elapsed=monotonic_us()-begin;
                pthread_mutex_lock(&algorithm_lock);
                if(frame.epoch!=atomic_load(&media_epoch))goto stale_frame;
                access_observe_hand(raw_found,ui_monotonic_ms());
                pthread_mutex_unlock(&algorithm_lock);
                FacePoseRes_t face_pose={0};
                int raw_face=frame.have_face?facepose_detect(frame.face_nv12,HAND_INPUT_WIDTH,HAND_INPUT_HEIGHT,&face_pose):-1;
                pthread_mutex_lock(&algorithm_lock);
                if(frame.epoch!=atomic_load(&media_epoch))goto stale_frame;
                /* Detect both on their own camera data, regardless of priority.
                 * Only the raw hand result selects the recognition modality. */
                FacePoseRes_t face_copy=face_pose;face_pose.sharpness=-2;
                if(frame.have_face){++face_frames;if(raw_face>0)++face_hits;else if(raw_face<0)++face_errors;}
                uint64_t elapsed=monotonic_us()-begin;
                /* Publish geometry before any database or result processing.
                 * Feature quality is a separate, recent-session annotation. */
                HandOverlay early_hand,early_face;
                HandPoseRes_t early_pose=pose;early_pose.status=-1;
                FacePoseRes_t early_face_pose=face_pose;early_face_pose.status=-1;
                int early_h=raw_found>0&&!hand_map(&early_pose,&frame.view,&early_hand);
                int early_f=raw_face>0&&!face_map(&early_face_pose,&frame.view,&early_face);
                pthread_mutex_lock(&video_lock);
                if(early_h&&overlay_state.valid)early_hand.status=overlay_state.drawing.status;
                if(early_f&&face_overlay_state.valid)early_face.status=face_overlay_state.drawing.status;
                hand_overlay_update(&overlay_state,early_h?&early_hand:NULL,raw_found==0?0:early_h?1:-1,ui_monotonic_ms());
                hand_overlay_update(&face_overlay_state,early_f?&early_face:NULL,raw_face==0?0:early_f?1:-1,ui_monotonic_ms());
                overlay_epoch=frame.epoch;
                pthread_mutex_unlock(&video_lock);
                hand_time_us+=hand_elapsed;++hand_frames;perf_add(&algorithm_perf[0],elapsed);
                /* The algorithm owns its YUV buffer until this whole call is
                 * finished. Drawing gets a separate, unmodified geometry copy. */
                HandPoseRes_t draw_copy=pose;begin=monotonic_us();
                access_set_ir_sample(&frame.ir_sample);
                access_set_face_pair(&frame.face_pair);
                int calibrating=face_ir_calibration_active();
                if(calibrating) {
                    if(!calibration_was_active)access_detection_gap();
                    PvaFaceGeometry ir_geometry={0};
                    int ir_found=raw_face>0&&frame.have_ir&&face_ir_pair_valid(&frame.face_pair)?
                        facepose_detect_ir(frame.yuv,HAND_INPUT_WIDTH,HAND_INPUT_HEIGHT,&ir_geometry):0;
                    face_ir_calibration_feed(raw_face>0?&face_pose:NULL,ir_found>0?&ir_geometry:NULL,
                        HAND_INPUT_WIDTH,HAND_INPUT_HEIGHT,&frame.face_pair,ui_monotonic_ms());
                } else {
                    if(calibration_was_active){access_detection_gap();printf("FACE_IR_CAL resumed\n");}
                    access_biometrics_frame(frame.yuv,frame.face_nv12,HAND_INPUT_WIDTH,HAND_INPUT_HEIGHT,
                        &pose,raw_found,&face_pose,raw_face,frame.source==CAMERA_RGB,ui_monotonic_ms());
                }
                calibration_was_active=calibrating;
                int found=raw_found>0;
                face_copy.status=face_pose.status==STATUS_OK&&face_pose.sharpness==-2?-1:face_pose.status;
                HandOverlay face_drawing;
                int face_visible=raw_face>0&&!face_map(&face_copy,&frame.view,&face_drawing);
                perf_add(&algorithm_perf[1],monotonic_us()-begin);
                draw_copy.status=pose.status;
                if(found!=previous_found||(found>0&&pose.status!=previous_quality)) {
                    printf("PALM_QUALITY found=%d status=%d sample=%s raw_found=%d age_ms=%llu\n",found,pose.status,
                        found<=0?"hidden":pose.status==STATUS_OK?"green":"red",raw_found,
                        (unsigned long long)(ui_monotonic_ms()-frame.captured_ms));
                    previous_found=found;previous_quality=pose.status;
                }
                perf_add(&algorithm_perf[2],(ui_monotonic_ms()-frame.captured_ms)*1000);
                HandOverlay drawing;int visible=found>0&&!hand_map(&draw_copy,&frame.view,&drawing);
                pthread_mutex_lock(&video_lock);
                /* Do not count a miss twice or replace this frame's geometry
                 * with coordinates belonging to a completed background job. */
                if(visible&&overlay_state.valid)overlay_state.drawing.status=drawing.status;
                if(face_visible&&face_overlay_state.valid)face_overlay_state.drawing.status=face_drawing.status;
                pthread_mutex_unlock(&video_lock);
                if(raw_found<0){if(frame.have_ir)atomic_store(&hand_ready,0);frame_hand_state=-1;}
                else {frame_hand_state=found?2:1;if(found)++hand_hits;}
                if(frame.dump) {
                    if(face_visible)face_draw(frame.rgb,UI_WIDTH,UI_HEIGHT,&face_drawing);
                    if(visible)hand_draw(frame.rgb,UI_WIDTH,UI_HEIGHT,&drawing);
                    dump_hand_frame(frame.yuv,frame.rgb,&draw_copy,found,frame.source,&frame.view);
                }
            }
        }
        goto frame_done;
stale_frame:
        access_detection_gap();hide_overlay();
frame_done:
        now=ui_monotonic_ms();
        access_idle_maintenance(now);
        if(now>=hand_log) {
            unsigned n=hand_frames-last_hand_frames;
            printf("PALM_STATS source=IR pipe=0 input=540x960 Y-only uncropped frames=%u hits=%u infer_ms=%.2f state=%d\n",
                n,hand_hits-last_hand_hits,n?(hand_time_us-last_hand_us)/(1000.0*n):0,frame_hand_state);
            printf("FACE_STATS source=RGB pipe=1 input=540x960 NV12 frames=%u hits=%u errors=%u\n",face_frames,face_hits,face_errors);
            access_log_pipeline();perf_log("biometric.detect",&algorithm_perf[0]);perf_log("palm.access",&algorithm_perf[1]);perf_log("palm.age",&algorithm_perf[2]);
            pthread_mutex_lock(&palm_queue.lock);
            printf("PALM_QUEUE posted=%u taken=%u replaced=%u depth=%d\n",palm_queue.posted,palm_queue.taken,palm_queue.replaced,palm_queue.ready);
            pthread_mutex_unlock(&palm_queue.lock);
            last_hand_frames=hand_frames;last_hand_hits=hand_hits;last_hand_us=hand_time_us;hand_log=now+5000;
        }
        pthread_mutex_unlock(&algorithm_lock);
        if(!have)usleep(3000);
    }
    atomic_store(&result_stop,1);if(results_started)pthread_join(results,NULL);
    palm_frame_free(&frame);return NULL;
}
static int capture_rgb(enum camera_source source,unsigned char *rgb,unsigned char *ir_yuv,unsigned char *face_nv12)
{
    IrExposureSample ir_sample={0};FaceIrPair pair={0};
    VIDEO_FRAME_INFO_S f={0};int have_ir=0,have_face=0;uint64_t tick=monotonic_us();
    int rc=media_camera_get(&f);
    if(rc){atomic_store(&detection_gap,1);hide_overlay();return rc;}
    uint64_t captured_ms=ui_monotonic_ms();
    if(source==CAMERA_IR)pair.ir_pts_us=f.stVFrame.u64PTS;
    else if(source==CAMERA_RGB)pair.rgb_pts_us=f.stVFrame.u64PTS;
    if(source==CAMERA_IR&&atomic_load(&hand_ready))media_ir_sample(&f,&ir_sample);
    perf_add(&cap_perf[0],monotonic_us()-tick);tick=monotonic_us();
    HandViewport view={0};
    rc=media_scale_frame(&f,rgb,&view,source==CAMERA_IR);
    perf_add(&cap_perf[1],monotonic_us()-tick);tick=monotonic_us();
    if(!rc&&source==CAMERA_IR&&atomic_load(&hand_ready))have_ir=media_ir_half_frame(&f,ir_yuv)==0;
    if(!rc&&source==CAMERA_RGB&&atomic_load(&hand_ready)&&face_enabled)
        have_face=media_rgb_half_frame(&f,face_nv12)==0;
    if(media_camera_release(&f))rc=-1;
    if(rc){atomic_store(&detection_gap,1);hide_overlay();return rc;}
    if(source==CAMERA_RGB&&atomic_load(&hand_ready)) {
        memset(&f,0,sizeof(f));int ir_rc=media_camera_get_ir(&f);
        if(!ir_rc) {
            pair.ir_pts_us=f.stVFrame.u64PTS;
            media_ir_sample(&f,&ir_sample);
            ir_rc=media_ir_half_frame(&f,ir_yuv);
            if(media_camera_release_ir(&f))ir_rc=-1;
        }
        have_ir=!ir_rc;
    }
    if(source==CAMERA_IR&&atomic_load(&hand_ready)&&face_enabled) {
        memset(&f,0,sizeof(f));int face_rc=media_camera_get_rgb(&f);
        if(!face_rc) {
            pair.rgb_pts_us=f.stVFrame.u64PTS;
            face_rc=media_rgb_half_frame(&f,face_nv12);
            if(media_camera_release_rgb(&f))face_rc=-1;
        }
        have_face=!face_rc;
    }
    perf_add(&cap_perf[2],monotonic_us()-tick);
    if(atomic_load(&hand_ready)&&source!=CAMERA_DVP&&!have_ir) {
        if(++ir_failures==1||ir_failures%100==0)fprintf(stderr,"PALM IR frame unavailable count=%u; overlay cleared\n",ir_failures);
    }
    if(!have_ir&&!have_face){atomic_store(&detection_gap,1);hide_overlay();}
    else {
        int dump=atomic_exchange(&hand_dump_requested,0);
        if(!palm_queue_put_timed_pair(&palm_queue,have_ir?ir_yuv:NULL,have_face?face_nv12:NULL,rgb,&view,source,atomic_load(&media_epoch),captured_ms,dump,&ir_sample,&pair)&&dump)
            atomic_store(&hand_dump_requested,1);
    }
    return 0;
}
static void publish_frame(const unsigned char *rgb)
{
    pthread_mutex_lock(&video_lock);
    uint64_t published=ui_monotonic_ms();
    uint64_t interval=video_ms?published-video_ms:0;
    if(video_ms)perf_add(&cap_perf[3],(published-video_ms)*1000);
    memcpy(latest,rgb,UI_WIDTH*UI_HEIGHT*3);video_ms=published;++frames;
    pthread_mutex_unlock(&video_lock);
    if(trace_stall&&interval>150)printf("CAPTURE_GAP uptime_ms=%llu interval_ms=%llu\n",
        (unsigned long long)published,(unsigned long long)interval);
}
static int start_source(enum camera_source source,unsigned char *rgb,unsigned char *ir_yuv,unsigned char *face_nv12)
{
    int init=media_camera_start(source);
    if(init) return init;
    if(media_scale_start(source)) return -1;
    uint64_t deadline=ui_monotonic_ms()+3000;
    while(!atomic_load(&worker_stop)&&ui_monotonic_ms()<deadline) {
        if(!capture_rgb(source,rgb,ir_yuv,face_nv12)) return 0;
    }
    fprintf(stderr,"SOURCE %s first-frame timeout\n",camera_source_name(source));return -1;
}
/* The state lock protects ISP callbacks and source lifecycle transitions.
 * Pose may finish on an owned old image, but that epoch cannot enter business. */
static void invalidate_algorithm_input(void)
{
    atomic_fetch_add(&media_epoch,1);palm_queue_clear(&palm_queue);hide_overlay();
    access_detection_gap();
}
static void stop_source(void)
{
    pthread_mutex_lock(&algorithm_lock);invalidate_algorithm_input();
    media_scale_stop();media_camera_stop();access_exposure_reset();
    pthread_mutex_unlock(&algorithm_lock);
}
static void *capture_worker(void *unused)
{
    (void)unused;
    unsigned char *rgb=malloc(UI_WIDTH*UI_HEIGHT*3);
    unsigned char *ir_yuv=malloc(HAND_INPUT_WIDTH*HAND_INPUT_HEIGHT*3/2);
    unsigned char *face_nv12=malloc(HAND_INPUT_WIDTH*HAND_INPUT_HEIGHT*3/2);
    unsigned failures=0;int current=-1,attempted=0,inference_started=0;pthread_t inference;
    if(!rgb||!ir_yuv||!face_nv12) { free(rgb);free(ir_yuv);free(face_nv12);atomic_store(&worker_stop,2);return NULL; }
    if(palm_queue_init(&palm_queue)){free(rgb);free(ir_yuv);free(face_nv12);atomic_store(&worker_stop,2);return NULL;}
    /* API remains YUV. UV is neutral and ignored by the CV184X library. */
    memset(ir_yuv+HAND_INPUT_WIDTH*HAND_INPUT_HEIGHT,128,HAND_INPUT_WIDTH*HAND_INPUT_HEIGHT/2);
    hand_ready=hand_enabled&&handpose_start(pva_config)==0;
    face_enabled=hand_ready&&facepose_enabled();
    access_open_biometrics(user_data_dir,hand_ready?&ky50_palm_backend:NULL,face_enabled?&ky50_face_backend:NULL);
    face_ir_calibration_init(user_data_dir,"assets/face_ir_calibration.conf");
    if(hand_ready)access_set_exposure_backend(&media_ir_exposure_backend);
    if(pthread_create(&inference,NULL,inference_worker,NULL))atomic_store(&worker_stop,2);
    else inference_started=1;
    uint64_t capture_log=ui_monotonic_ms()+5000;
    while(!atomic_load(&worker_stop)) {
        int target=atomic_exchange(&source_request,-1);
        if(target>=0) {
            int transient=target&0x100;target&=0xff;
            int previous=current,boot=!attempted,rc,result;
            atomic_store(&source_busy,1);
            int select_only=attempted&&current>=0&&current!=CAMERA_DVP&&target!=CAMERA_DVP;
            if(attempted&&!select_only) stop_source();
            attempted=1;
            printf("SOURCE switching %s -> %s\n",previous<0?"none":camera_source_name(previous),camera_source_name(target));
            pthread_mutex_lock(&video_lock);video_ms=0;pthread_mutex_unlock(&video_lock);
            if(select_only) {
                pthread_mutex_lock(&algorithm_lock);invalidate_algorithm_input();
                rc=media_camera_select_preview(target);pthread_mutex_unlock(&algorithm_lock);
                if(!rc)rc=capture_rgb(target,rgb,ir_yuv,face_nv12);
            } else rc=start_source(target,rgb,ir_yuv,face_nv12);
            if(!rc) {
                current=target;publish_frame(rgb);
                rc=(boot||transient)?0:settings_save(settings_dir,current);
                result=rc?2:1;
            } else {
                if(!select_only)stop_source();
                current=-1;
                int fallback=previous<0?CAMERA_RGB:previous;
                if(target!=fallback || previous>=0) {
                    int restored=select_only?media_camera_select_preview(fallback):start_source(fallback,rgb,ir_yuv,face_nv12);
                    if(select_only&&!restored)restored=capture_rgb(fallback,rgb,ir_yuv,face_nv12);
                    if(!restored) { current=fallback;publish_frame(rgb); }
                    else stop_source();
                }
                result=current<0?4:rc==-2?5:3;
            }
            atomic_store(&source_current,current);atomic_store(&source_busy,0);
            atomic_store(&source_result,result);
            failures=0;
        }
        if(current<0) { usleep(10000);continue; }
        int rc=capture_rgb(current,rgb,ir_yuv,face_nv12);
        if(rc) {
            if(++failures==1||failures%100==0) fprintf(stderr,"CAMERA unavailable count=%u rc=%#x\n",failures,rc);
            usleep(10000);continue;
        }
        failures=0;publish_frame(rgb);
        uint64_t now=ui_monotonic_ms();
        if(now>=capture_log) {
            const char *names[]={"capture.vi","capture.preview","capture.ir","capture.interval"};
            for(int i=0;i<4;i++)perf_log(names[i],&cap_perf[i]);
            capture_log=now+5000;
        }
    }
    atomic_store(&inference_stop,1);if(inference_started)pthread_join(inference,NULL);
    access_close();handpose_stop();
    if(attempted)stop_source();
    palm_queue_destroy(&palm_queue);
    free(face_nv12);free(ir_yuv);free(rgb);return NULL;
}
int main(int argc,char **argv)
{
    const char *event=NULL;int test=0,override=-1;unsigned duration=0;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--touch-test")) test=1;
        else if(!strcmp(argv[i],"--no-handpose")) hand_enabled=0;
        else if(!strcmp(argv[i],"--pva-config")&&i+1<argc) pva_config=argv[++i];
        else if(!strcmp(argv[i],"--data-dir")&&i+1<argc) user_data_dir=argv[++i];
        else if(!strcmp(argv[i],"--event")&&i+1<argc) event=argv[++i];
        else if(!strcmp(argv[i],"--source")&&i+1<argc) {
            const char *name=argv[++i];
            for(int k=0;k<CAMERA_SOURCE_COUNT;k++) if(!strcmp(name,camera_source_name(k))) override=k;
            if(override<0) return 2;
        }
        else if(!strcmp(argv[i],"--config-dir")&&i+1<argc) settings_dir=argv[++i];
        else if(!strcmp(argv[i],"--seconds")&&i+1<argc) duration=strtoul(argv[++i],NULL,10);
        else { fprintf(stderr,"Usage: %s [--event /dev/input/eventN] [--touch-test] [--seconds N] [--source RGB|IR|DVP] [--config-dir DIR] [--data-dir DIR] [--pva-config FILE] [--no-handpose]\n",argv[0]);return 2; }
    }
    setvbuf(stdout,NULL,_IOLBF,0);
    trace_stall=getenv("KY50_TRACE_STALL")!=NULL;
    signal(SIGINT,signal_stop);signal(SIGTERM,signal_stop);signal(SIGHUP,signal_stop);
    signal(SIGUSR1,signal_snapshot);signal(SIGUSR2,signal_page_test);
    int rc=1,fd=-1,lock=-1,cam=0,disp=0,ui=0,thread=0;
    pthread_t worker;
    unsigned char *rgb=calloc(UI_WIDTH*UI_HEIGHT,3);
    latest=calloc(UI_WIDTH*UI_HEIGHT,3);
    if(!rgb||!latest) goto done;
    lock=open("/tmp/ky50-sensor.lock",O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB)) { perror("sensor lock");goto done; }
    enum camera_source initial;
    if(settings_load(settings_dir,&initial)) fprintf(stderr,"Invalid source settings; using RGB\n");
    if(override>=0) initial=override;
    atomic_store(&source_request,initial);atomic_store(&source_current,-1);atomic_store(&source_busy,1);
    cam=1;if(media_system_start()) goto done;
    disp=1;if(media_display_start()) goto done;
    if(touch_prepare()) goto done;
    fd=touch_open(event);if(fd<0) goto done;
    if(ui_init()) goto done;
    ui=1;ui_set_source(camera_source_name(initial));if(test) ui_show(UI_TOUCH_TEST,0);
    /* Device open starts both low; apply the application default once. */
    if(!factory_lights_init()) factory_light_toggle(FACTORY_LIGHT_IR);
    ui_factory_lights(factory_light_state(FACTORY_LIGHT_WHITE),factory_light_state(FACTORY_LIGHT_IR));
    if(pthread_create(&worker,NULL,capture_worker,NULL)) goto done;
    thread=1;
    uint64_t start=ui_monotonic_ms(),next=0,net_next=0,log_last=start,log_next=start+5000,last_submit_ms=0;
    int eth=-1,last_draw=-1;unsigned shown=0,last_frames=0,last_shown=0;
    unsigned presented_frame=(unsigned)-1,presented_ui=(unsigned)-1;
    int presented_good=-1,presented_hand=0,presented_face=0;
    unsigned relay_sequence=0,presented_success=0;
    HandOverlay presented_hand_box={0},presented_face_box={0};
    enum ui_page last_page=ui_current_page();int before_enrollment=-1,before_calibration=-1;
    printf("KY50_UI LVGL 9.6.0 RGB/IR/DVP CCW90 480x854, IR pipe0 palm + RGB pipe1 face; dual overlays, raw palm priority\n");
    rc=0;
    while(!quit&&!atomic_load(&worker_stop)) {
        uint64_t now=ui_monotonic_ms();
        if(duration&&now-start>=(uint64_t)duration*1000) break;
        if(touch_poll(fd)) { fprintf(stderr,"Touch device disconnected\n");rc=1;break; }
        /* Pull success before choosing/copying this render's clean RGB frame.
         * Keep all LVGL work on this thread and preserve VO's minimum spacing. */
        ui_access_poll();
        if(ui_access_state()->success_sequence!=presented_success&&next>last_submit_ms+20)
            next=last_submit_ms+20;
        if(now>=net_next) {
            int carrier=ui_carrier("/sys/class/net/eth0/carrier");
            if(carrier!=eth) printf("ETHERNET carrier=%d uptime_ms=%llu\n",carrier,(unsigned long long)now);
            eth=carrier;net_next=now+500;
        }
        pthread_mutex_lock(&video_lock);
        int good=video_ms&&(now<=video_ms||now-video_ms<1000);
        uint64_t camera_age_ms=now>=video_ms?now-video_ms:0;
        unsigned count=frames;
        HandOverlay drawing={0},face_drawing={0};
        int draw_face=overlay_epoch==atomic_load(&media_epoch)&&hand_overlay_snapshot(&face_overlay_state,now,&face_drawing);
        int draw=overlay_epoch==atomic_load(&media_epoch)&&hand_overlay_snapshot(&overlay_state,now,&drawing);
        unsigned draw_misses=overlay_state.missing;
        uint64_t draw_age=now>=overlay_state.updated_ms?now-overlay_state.updated_ms:0;
        if(now>=next)memcpy(rgb,latest,UI_WIDTH*UI_HEIGHT*3);
        pthread_mutex_unlock(&video_lock);
        if(now>=next) {
            if(last_draw!=(draw&&good)) {
                last_draw=draw&&good;
                printf("PALM_DRAW visible=%d misses=%u result_age_ms=%llu\n",last_draw,draw_misses,(unsigned long long)draw_age);
            }
        }
        uint64_t process_begin=monotonic_us();
        ui_update(eth,time(NULL),good);ui_process();ui_access_poll();
        uint64_t process_us=monotonic_us()-process_begin;
        perf_add(&ui_perf[2],process_us);
        if(trace_stall&&process_us>80000)printf("UI_SLOW stage=process uptime_ms=%llu elapsed_us=%llu\n",
            (unsigned long long)(process_begin/1000),(unsigned long long)process_us);
        enum ui_page active_page=ui_current_page();
        if(active_page==UI_PALM_ENROLL&&last_page!=UI_PALM_ENROLL) {
            before_enrollment=atomic_load(&source_current);
            if(before_enrollment<0)before_enrollment=initial;
            atomic_store(&source_request,CAMERA_RGB|0x100);
        } else if(active_page!=UI_PALM_ENROLL&&last_page==UI_PALM_ENROLL&&before_enrollment>=0) {
            atomic_store(&source_request,before_enrollment|0x100);before_enrollment=-1;
        }
        if(active_page!=UI_FACE_IR_CALIBRATE&&last_page==UI_FACE_IR_CALIBRATE) {
            face_ir_calibration_cancel();
            if(before_calibration>=0)atomic_store(&source_request,before_calibration|0x100);
            before_calibration=-1;
        }
        last_page=active_page;
        enum ui_action action=ui_take_action();
        if(action>=UI_ACTION_RGB && action<=UI_ACTION_DVP) {
            if(atomic_load(&source_busy)) ui_factory_status("正在切换，请稍候");
            else {
                int target=action-UI_ACTION_RGB;
                if(target==atomic_load(&source_current)) {
                    if(settings_save(settings_dir,target)) ui_factory_status("配置保存失败");
                    else { ui_factory_status("当前显示已保存");ui_show(UI_CAMERA_PREVIEW,0); }
                } else {
                    atomic_store(&source_busy,1);atomic_store(&source_request,target);
                    ui_factory_status("正在切换，请稍候");
                }
            }
        } else if(action==UI_ACTION_FACE_IR_CALIBRATE) {
            if(atomic_load(&source_busy)||!atomic_load(&hand_ready)||!face_enabled)
                ui_factory_status("人脸或摄像头尚未就绪");
            else if(face_ir_calibration_begin(now))ui_factory_status("校准模块尚未就绪");
            else {
                before_calibration=atomic_load(&source_current);
                if(factory_light_state(FACTORY_LIGHT_IR)==0)factory_light_toggle(FACTORY_LIGHT_IR);
                if(before_calibration!=CAMERA_RGB)atomic_store(&source_request,CAMERA_RGB|0x100);
                ui_show(UI_FACE_IR_CALIBRATE,0);last_page=UI_FACE_IR_CALIBRATE;
            }
        } else if(action==UI_ACTION_RELAY) {
            ui_factory_status(factory_relay()?"继电器设备不可用或正在测试":"继电器打开，五秒后自动关闭");
        } else if(action==UI_ACTION_SPEAKER) {
            ui_factory_status(factory_speaker()?"正在播放，请稍候":"正在播放音频");
        } else if(action==UI_ACTION_WHITE_LIGHT||action==UI_ACTION_IR_LIGHT) {
            enum factory_light light=action==UI_ACTION_IR_LIGHT?FACTORY_LIGHT_IR:FACTORY_LIGHT_WHITE;
            int on=factory_light_toggle(light);
            ui_factory_lights(factory_light_state(FACTORY_LIGHT_WHITE),factory_light_state(FACTORY_LIGHT_IR));
            if(on<0) ui_factory_status("灯控设备不可用或操作失败");
            else if(light==FACTORY_LIGHT_IR) ui_factory_status(on?"红外灯已开启":"红外灯已关闭");
            else ui_factory_status(on?"白光灯已开启":"白光灯已关闭");
        }
        int result=atomic_exchange(&source_result,0);
        if(result) {
            int source=atomic_load(&source_current);
            if(source>=0) ui_set_source(camera_source_name(source));
            if(result==1&&ui_current_page()==UI_FACTORY) ui_show(UI_CAMERA_PREVIEW,0);
            static const char *messages[]={"","显示切换成功","显示成功，配置保存失败",
                "切换失败，已恢复原画面","摄像头不可用","DVP时钟连线需调整，已恢复原画面"};
            ui_factory_status(messages[result]);
        }
        if(ui_current_page()==UI_FACE_IR_CALIBRATE) {
            FaceIrCalibrationStatus calibration;face_ir_calibration_status(now,&calibration);
            ui_face_ir_calibration_status(&calibration);
        }
        const char *factory_message=factory_poll();
        if(factory_message) ui_factory_status(factory_message);
        if(page_test_step(now)) rc=1;
        if(now>=next) {
            const AccessSnapshot *state=ui_access_state();
            int enrolling=ui_current_page()==UI_PALM_ENROLL;
            drawing.status=hand_overlay_display_status(drawing.status,state->verified_modalities&CREDENTIAL_PALM,enrolling);
            face_drawing.status=hand_overlay_display_status(face_drawing.status,state->verified_modalities&CREDENTIAL_FACE,enrolling);
            if(draw_face&&good)face_draw(rgb,UI_WIDTH,UI_HEIGHT,&face_drawing);
            if(draw&&good)hand_draw(rgb,UI_WIDTH,UI_HEIGHT,&drawing);
            if(state->success_sequence!=relay_sequence) {
                relay_sequence=state->success_sequence;
                int pulse=relay_control_pulse(state->success_relay_seconds);
                printf("ACCESS_RELAY sequence=%u modality=%u seconds=%d rc=%d age_ms=%llu\n",
                    relay_sequence,state->success_modality,state->success_relay_seconds,pulse,
                    (unsigned long long)(ui_monotonic_ms()-state->success_ms));
            }
            /* Submit fully updated labels and button states, not a snapshot
             * still waiting for LVGL's next refresh timer. */
            uint64_t render_begin=monotonic_us();ui_render();
            uint64_t render_us=monotonic_us()-render_begin;
            perf_add(&ui_perf[0],render_us);
            if(trace_stall&&render_us>80000)printf("UI_SLOW stage=render uptime_ms=%llu elapsed_us=%llu\n",
                (unsigned long long)(render_begin/1000),(unsigned long long)render_us);
            if(snapshot_requested) {
                snapshot_requested=0;atomic_store(&hand_dump_requested,1);media_display_snapshot("/tmp/ky50-ui/screen.ppm");
            }
            /* Keep the regular 33 ms preparation cadence, but never catch up
             * with a burst after a slow redraw. VO has a one-frame wait queue;
             * the 20 ms lower bound leaves more than one 60 Hz scan period. */
            uint64_t submit=ui_monotonic_ms();
            const unsigned generation=ui_generation();
            const int show_hand=draw&&good,show_face=draw_face&&good;
            uint64_t touch=touch_pending_ms();
            /* VO retains the current image. Reblending an identical camera,
             * UI and overlay wastes CPU needed by detection/feature work. */
            int changed=count!=presented_frame||generation!=presented_ui||good!=presented_good||
                show_hand!=presented_hand||show_face!=presented_face||touch||state->success_sequence!=presented_success||
                (show_hand&&memcmp(&drawing,&presented_hand_box,sizeof(drawing)))||
                (show_face&&memcmp(&face_drawing,&presented_face_box,sizeof(face_drawing)));
            if(changed) {
                last_submit_ms=submit;
                static uint64_t last_present_us;
                uint64_t display_begin=monotonic_us();
                if(media_display_present(rgb,ui_pixels())) { rc=1;break; }
                perf_add(&ui_perf[1],monotonic_us()-display_begin);
                uint64_t completed=monotonic_us();
                if(trace_stall&&last_present_us&&completed-last_present_us>150000)
                    printf("PREVIEW_GAP uptime_ms=%llu interval_us=%llu present_us=%llu render_us=%llu camera_age_ms=%llu page=%d\n",
                        (unsigned long long)(completed/1000),(unsigned long long)(completed-last_present_us),
                        (unsigned long long)(completed-display_begin),(unsigned long long)render_us,
                        (unsigned long long)camera_age_ms,ui_current_page());
                if(state->success_sequence!=presented_success) {
                    presented_success=state->success_sequence;
                    printf("ACCESS_PRESENT sequence=%u modality=%u age_ms=%llu green_hand=%d green_face=%d\n",
                        presented_success,state->success_modality,
                        (unsigned long long)(completed/1000-state->success_ms),
                        show_hand&&drawing.status==STATUS_OK,show_face&&face_drawing.status==STATUS_OK);
                }
                last_present_us=completed;
                presented_frame=count;presented_ui=generation;presented_good=good;
                presented_hand=show_hand;presented_face=show_face;
                if(show_hand)presented_hand_box=drawing;
                if(show_face)presented_face_box=face_drawing;
                if(touch) {
                    printf("TOUCH_SUBMIT latency_ms=%llu page=%d\n",
                        (unsigned long long)(ui_monotonic_ms()-touch),ui_current_page());
                    touch_presented();
                }
                ++shown;
            }
            next=now+33;
            if(next<submit+20) next=submit+20;
        }
        if(now>=log_next) {
            perf_log("ui.render",&ui_perf[0]);perf_log("ui.present",&ui_perf[1]);perf_log("ui.process",&ui_perf[2]);
            double seconds=(now-log_last)/1000.0;
            printf("STATS elapsed=%llu capture_fps=%.2f display_fps=%.2f frames=%u page=%d eth=%d\n",
                   (unsigned long long)(now-start)/1000,(count-last_frames)/seconds,
                   (shown-last_shown)/seconds,count,ui_current_page(),eth);
            last_frames=count;last_shown=shown;log_last=now;log_next=now+5000;
        }
        usleep(3000);
    }
    if(atomic_load(&worker_stop)==2) rc=1;
done:
    atomic_store(&worker_stop,1);
    if(thread) pthread_join(worker,NULL);
    if(ui) ui_destroy();
    factory_stop();
    if(disp) media_display_stop();
    if(cam) media_system_stop();
    if(fd>=0) close(fd);
    if(lock>=0) close(lock);
    free(latest);free(rgb);
    printf("KY50_UI stopped rc=%d\n",rc);return rc;
}
