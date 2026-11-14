#include "palm_queue.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define Y_BYTES (HAND_INPUT_WIDTH*HAND_INPUT_HEIGHT)
static PalmQueue q;
static atomic_int done;
static void *produce(void *arg)
{
    (void)arg;uint8_t *y=malloc(Y_BYTES),*face=malloc(Y_BYTES*3/2);assert(y&&face);HandViewport v={540,960,486,864,2,4};
    for(unsigned i=1;i<=600;i++) {
        memset(y,i%251,Y_BYTES);
        memset(face,(i+81)%251,Y_BYTES*3/2);
        FaceIrPair pair={i*33333u,i*33333u+10000u};
        assert(palm_queue_put_timed_pair(&q,y,face,NULL,&v,0,3,i,0,&(IrExposureSample){.sequence=i,.valid=1},&pair));
        if(i%20==0)usleep(100);
    }
    free(face);free(y);atomic_store(&done,1);return NULL;
}
int main(void)
{
    PalmFrame f;assert(!palm_queue_init(&q));assert(!palm_frame_init(&f));
    pthread_t p;assert(!pthread_create(&p,NULL,produce,NULL));unsigned seen=0,last=0;
    for(;;) {
        if(!palm_queue_take(&q,&f)){if(atomic_load(&done))break;usleep(100);continue;}
        assert(f.captured_ms>last&&f.epoch==3&&!f.dump);last=f.captured_ms;++seen;
        /* Consumer sleeps while capture overwrites its pending slot. */
        usleep(200);
        assert(f.have_ir&&f.have_face);
        assert(f.ir_sample.valid&&f.ir_sample.sequence==f.captured_ms);
        assert(f.face_pair.rgb_pts_us==f.captured_ms*33333&&f.face_pair.ir_pts_us==f.captured_ms*33333+10000);
        for(unsigned i=0;i<Y_BYTES*3/2;i++)assert(f.face_nv12[i]==(last+81)%251);
        for(unsigned i=0;i<Y_BYTES;i++)assert(f.yuv[i]==last%251);
        for(unsigned i=Y_BYTES;i<Y_BYTES*3/2;i++)assert(f.yuv[i]==128);
    }
    pthread_join(p,NULL);assert(last==600&&seen>0&&q.replaced>0);
    uint8_t *y=calloc(Y_BYTES,1),*rgb=malloc(480*854*3);assert(y&&rgb);
    memset(rgb,0xa5,480*854*3);HandViewport v={540,960,486,864,2,4};
    assert(palm_queue_put(&q,y,rgb,&v,1,4,1000,1));
    assert(!palm_queue_put(&q,y,NULL,&v,0,4,1001,0));
    assert(palm_queue_take(&q,&f)&&f.dump&&f.source==1&&f.captured_ms==1000);
    assert(f.have_ir&&!f.have_face);
    assert(!f.face_pair.rgb_pts_us&&!f.face_pair.ir_pts_us);
    for(unsigned i=0;i<480*854*3;i++)assert(f.rgb[i]==0xa5);
    assert(palm_queue_put(&q,y,NULL,&v,0,5,1002,0));palm_queue_clear(&q);
    assert(!palm_queue_take(&q,&f));
    free(rgb);free(y);palm_frame_free(&f);palm_queue_destroy(&q);
    puts("PASS: concurrent latest-frame replacement, immutable in-flight IR Y and RGB full NV12, neutral UV, exact paired snapshot, source epoch metadata and clear");
    return 0;
}
