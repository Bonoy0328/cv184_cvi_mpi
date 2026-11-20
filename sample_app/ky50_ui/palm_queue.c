#include "palm_queue.h"
#include <stdlib.h>
#include <string.h>
#define Y_BYTES (HAND_INPUT_WIDTH*HAND_INPUT_HEIGHT)
#define RGB_BYTES (480*854*3)
int palm_frame_init(PalmFrame *f)
{
    memset(f,0,sizeof(*f));f->yuv=malloc(Y_BYTES*3/2);f->rgb=malloc(RGB_BYTES);f->face_nv12=malloc(Y_BYTES*3/2);
    f->finger_gray=malloc(FINGER_BYTES);
    if(!f->yuv||!f->rgb||!f->face_nv12||!f->finger_gray){palm_frame_free(f);return -1;}
    memset(f->yuv+Y_BYTES,128,Y_BYTES/2);return 0;
}
void palm_frame_free(PalmFrame *f) {free(f->yuv);free(f->rgb);free(f->face_nv12);free(f->finger_gray);memset(f,0,sizeof(*f));}
int palm_queue_init(PalmQueue *q)
{
    memset(q,0,sizeof(*q));if(pthread_mutex_init(&q->lock,NULL))return -1;
    if(palm_frame_init(&q->pending)){pthread_mutex_destroy(&q->lock);return -1;}return 0;
}
void palm_queue_destroy(PalmQueue *q){palm_frame_free(&q->pending);pthread_mutex_destroy(&q->lock);}
void palm_queue_clear(PalmQueue *q){pthread_mutex_lock(&q->lock);q->ready=0;pthread_mutex_unlock(&q->lock);}
int palm_queue_put(PalmQueue *q,const unsigned char *yuv,const unsigned char *rgb,
                   const HandViewport *view,int source,unsigned epoch,uint64_t ms,int dump)
{
    return palm_queue_put_pair(q,yuv,NULL,rgb,view,source,epoch,ms,dump,NULL);
}
int palm_queue_put_pair(PalmQueue *q,const unsigned char *yuv,const unsigned char *face_nv12,
                        const unsigned char *rgb,const HandViewport *view,int source,
                        unsigned epoch,uint64_t ms,int dump,const IrExposureSample *sample)
{
    return palm_queue_put_timed_pair(q,yuv,face_nv12,rgb,view,source,epoch,ms,dump,sample,NULL);
}
int palm_queue_put_timed_pair(PalmQueue *q,const unsigned char *yuv,const unsigned char *face_nv12,
                        const unsigned char *rgb,const HandViewport *view,int source,
                        unsigned epoch,uint64_t ms,int dump,const IrExposureSample *sample,const FaceIrPair *pair)
{
    return palm_queue_put_three(q,yuv,face_nv12,rgb,view,source,epoch,ms,dump,sample,pair,NULL,0);
}
int palm_queue_put_three(PalmQueue *q,const unsigned char *yuv,const unsigned char *face_nv12,
 const unsigned char *rgb,const HandViewport *view,int source,unsigned epoch,uint64_t ms,int dump,
 const IrExposureSample *sample,const FaceIrPair *pair,const uint8_t *finger_gray,uint64_t finger_id)
{
    pthread_mutex_lock(&q->lock);
    /* Preserve an explicitly requested paired diagnostic snapshot until taken. */
    if(q->ready&&q->pending.dump){pthread_mutex_unlock(&q->lock);return 0;}
    if(q->ready)++q->replaced;
    q->pending.have_finger=finger_gray!=NULL;q->pending.finger_id=finger_id;
    if(finger_gray)memcpy(q->pending.finger_gray,finger_gray,FINGER_BYTES);
    q->pending.have_ir=yuv!=NULL;q->pending.have_face=face_nv12!=NULL;
    if(yuv)memcpy(q->pending.yuv,yuv,Y_BYTES);
    if(face_nv12)memcpy(q->pending.face_nv12,face_nv12,Y_BYTES*3/2);
    if(dump)memcpy(q->pending.rgb,rgb,RGB_BYTES);
    q->pending.view=*view;q->pending.source=source;q->pending.epoch=epoch;
    q->pending.captured_ms=ms;q->pending.dump=dump;q->ready=1;++q->posted;
    q->pending.ir_sample=sample?*sample:(IrExposureSample){0};
    q->pending.face_pair=pair?*pair:(FaceIrPair){0};
    pthread_mutex_unlock(&q->lock);return 1;
}
int palm_queue_take(PalmQueue *q,PalmFrame *f)
{
    pthread_mutex_lock(&q->lock);int ready=q->ready;
    if(ready){PalmFrame old=*f;*f=q->pending;q->pending=old;q->ready=0;++q->taken;}
    pthread_mutex_unlock(&q->lock);return ready;
}
