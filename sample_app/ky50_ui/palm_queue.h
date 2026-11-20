#ifndef KY50_PALM_QUEUE_H
#define KY50_PALM_QUEUE_H
#include <pthread.h>
#include "hand_overlay.h"
#include "fingerprint_image.h"
#include "ir_exposure.h"
#include "face_ir_calibration.h"
/* One pending frame, one consumer-owned frame. Replacing the pending frame
 * never modifies the image being used by PVA. A skipped frame is not a miss. */
typedef struct {
    unsigned char *yuv,*rgb,*face_nv12,*finger_gray;
    HandViewport view;
    uint64_t captured_ms;
    IrExposureSample ir_sample;
    FaceIrPair face_pair;
    unsigned epoch;
    int source,dump,have_ir,have_face,have_finger;
    uint64_t finger_id;
} PalmFrame;
typedef struct {
    pthread_mutex_t lock;
    PalmFrame pending;
    int ready;
    unsigned posted,taken,replaced;
} PalmQueue;
int palm_frame_init(PalmFrame *f);
void palm_frame_free(PalmFrame *f);
int palm_queue_init(PalmQueue *q);
void palm_queue_destroy(PalmQueue *q);
void palm_queue_clear(PalmQueue *q);
int palm_queue_put(PalmQueue *q,const unsigned char *yuv,const unsigned char *rgb,
                   const HandViewport *view,int source,unsigned epoch,uint64_t ms,int dump);
int palm_queue_put_pair(PalmQueue *q,const unsigned char *ir,const unsigned char *face_nv12,
                        const unsigned char *rgb,const HandViewport *view,int source,
                        unsigned epoch,uint64_t ms,int dump,const IrExposureSample *sample);
int palm_queue_take(PalmQueue *q,PalmFrame *f);
int palm_queue_put_timed_pair(PalmQueue *q,const unsigned char *ir,const unsigned char *face_nv12,
                        const unsigned char *rgb,const HandViewport *view,int source,
                        unsigned epoch,uint64_t ms,int dump,const IrExposureSample *sample,const FaceIrPair *pair);
int palm_queue_put_three(PalmQueue *q,const unsigned char *ir,const unsigned char *face_nv12,
 const unsigned char *rgb,const HandViewport *view,int source,unsigned epoch,uint64_t ms,int dump,
 const IrExposureSample *sample,const FaceIrPair *pair,const uint8_t *finger_gray,uint64_t finger_id);
#endif
