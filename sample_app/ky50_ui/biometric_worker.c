#include "biometric_worker.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>

static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake=PTHREAD_COND_INITIALIZER;
static pthread_cond_t result_ready=PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static const PalmBackend *palm;
static const FaceBackend *face;
static const FingerBackend *finger;
static int started,stop,state; /* 0 idle, 1 worker owns input, 2 result */
static unsigned generation,slot_generation;
static BioJob slot;
static uint8_t *input;
static void *sample;
static uint16_t width,height;
int bio_face_live_ir(const FaceBackend *backend,uint8_t *ir,uint16_t w,uint16_t h,
                     const FacePoseRes_t *pose,const FaceIrPair *pair)
{
    PvaFaceGeometry region;
    int mapped=face_ir_calibration_map(pose,w,h,pair,&region),result=0;
    if(ir&&backend) {
        if(mapped==1)result=backend->live_ir_region?backend->live_ir_region(ir,w,h,&region):0;
        else if(mapped==0)result=backend->live_ir?backend->live_ir(ir,w,h):0;
    }
    if(getenv("KY50_TRACE_STALL"))printf("BIO_IR_PATH mode=%s pass=%d\n",
        mapped>0?"mapped":mapped<0?"mapped_reject":"detector",result);
    return result;
}
static uint64_t us(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}
static void *run(void *unused)
{
    (void)unused;
    if(setpriority(PRIO_PROCESS,0,1))perror("BIO worker nice");
    pthread_mutex_lock(&mutex);
    while(!stop) {
        while(!stop&&state!=1)pthread_cond_wait(&wake,&mutex);
        if(stop)break;
        BioJob job=slot;unsigned ticket=slot_generation;
        uint8_t *image=input;void *snapshot=sample;uint16_t w=width,h=height;
        input=NULL;sample=NULL;
        pthread_mutex_unlock(&mutex);
        uint64_t begin=us();
        pthread_mutex_lock(&mutex);int cancelled=stop||ticket!=generation;pthread_mutex_unlock(&mutex);
        if(cancelled)job.result=STATUS_FAILED;
        else if(job.stage==BIO_AUTH)
            job.result=job.kind==CREDENTIAL_FINGER?finger->recognize(job.feature,job.feature_size,&job.user):job.kind==CREDENTIAL_FACE?face->recognize(job.feature,&job.user):palm->recognize(job.feature,&job.user);
        else if(job.kind==CREDENTIAL_FINGER)
            job.result=finger->features(image,w,h,job.sample_id,&job.finger,job.feature,sizeof(job.feature),&job.feature_size);
        else if(job.kind==CREDENTIAL_FACE)
            job.result=face->sample_features(snapshot,&job.face,job.feature);
        else job.result=palm->features(image,w,h,&job.hand,job.feature);
        if(job.result==STATUS_OK&&((job.stage==BIO_FACE_RECOGNIZE&&job.face.status==STATUS_OK)||
           (job.stage==BIO_PALM_RECOGNIZE&&job.hand.status==STATUS_OK)||
           (job.stage==BIO_FINGER_RECOGNIZE&&job.finger.status==STATUS_OK))) {
            pthread_mutex_lock(&mutex);cancelled=stop||ticket!=generation||us()-begin>1500000;pthread_mutex_unlock(&mutex);
            if(!cancelled) {
                job.auth_ran=1;
                job.auth_result=job.kind==CREDENTIAL_FINGER?finger->recognize(job.feature,job.feature_size,&job.user):job.kind==CREDENTIAL_FACE?face->recognize(job.feature,&job.user):palm->recognize(job.feature,&job.user);
            }
        }
        /* Only successful RGB candidates need the extra IR work. A false IR
         * result never reaches business confirmation, SQLite, audio or relay. */
        if(job.stage==BIO_FACE_RECOGNIZE&&job.require_ir_liveness&&job.auth_ran&&job.auth_result>0) {
            pthread_mutex_lock(&mutex);cancelled=stop||ticket!=generation||us()-begin>1500000;pthread_mutex_unlock(&mutex);
            uint64_t live_begin=us();
            job.ir_live_result=!cancelled?bio_face_live_ir(face,image,w,h,&job.face,&job.face_pair):0;
            job.ir_live_us=us()-live_begin;
            if(job.ir_live_result!=1)job.result=job.face.status=STATUS_FACE_LIVENESS_FAILED;
            if(getenv("KY50_TRACE_STALL"))printf("BIO_IR_LIVE rgb_us=%llu elapsed_us=%llu pass=%d sample_sequence=%llu uptime_ms=%llu\n",
                (unsigned long long)(live_begin-begin),(unsigned long long)job.ir_live_us,job.ir_live_result,
                (unsigned long long)job.ir_sample.sequence,(unsigned long long)(us()/1000));
        }
        if(getenv("KY50_TRACE_STALL"))printf("BIO_STAGE modality=%s stage=%s elapsed_us=%llu result=%d uptime_ms=%llu\n",
            job.kind==CREDENTIAL_FINGER?"fingerprint":job.kind==CREDENTIAL_FACE?"face":"palm",job.stage==BIO_AUTH?"sdk_auth":job.stage==BIO_FEATURE?"feature":"feature_auth",
            (unsigned long long)(us()-begin),job.result,(unsigned long long)(us()/1000));
        if(image){memset(image,0,(size_t)w*h*(job.kind==CREDENTIAL_FACE?3:2)/2);free(image);}
        if(snapshot)face->sample_free(snapshot);
        pthread_mutex_lock(&mutex);
        if(!stop&&ticket==generation){job.completed_us=us();slot=job;state=2;pthread_cond_signal(&result_ready);}
        else {memset(&slot,0,sizeof(slot));state=0;}
        memset(&job,0,sizeof(job));
    }
    pthread_mutex_unlock(&mutex);
    return NULL;
}
int bio_worker_start(const PalmBackend *p,const FaceBackend *f){return bio_worker_start_modalities(p,f,NULL);}
int bio_worker_start_modalities(const PalmBackend *p,const FaceBackend *f,const FingerBackend *fp)
{
    if(started)return -1;
    if((!p&&!f&&!fp)||(f&&(!f->sample_capture||!f->sample_features||!f->sample_free)))return -1;
    palm=p;face=f;finger=fp;stop=state=0;input=NULL;sample=NULL;++generation;
    if(pthread_create(&thread,NULL,run,NULL))return -1;
    started=1;return 0;
}
void bio_worker_stop(void)
{
    if(!started)return;
    pthread_mutex_lock(&mutex);stop=1;++generation;pthread_cond_signal(&wake);pthread_cond_broadcast(&result_ready);pthread_mutex_unlock(&mutex);
    pthread_join(thread,NULL);
    if(input){memset(input,0,(size_t)width*height*(slot.kind==CREDENTIAL_FACE?3:2)/2);free(input);input=NULL;}
    if(sample){face->sample_free(sample);sample=NULL;}
    memset(&slot,0,sizeof(slot));state=0;started=0;palm=NULL;face=NULL;
}
int bio_worker_busy(void)
{
    pthread_mutex_lock(&mutex);int busy=state!=0;pthread_mutex_unlock(&mutex);return busy;
}
void bio_worker_cancel(unsigned kind)
{
    pthread_mutex_lock(&mutex);
    if(state&&(!kind||slot.kind==kind)) {
        ++generation;
        if(state==2){state=0;memset(&slot,0,sizeof(slot));}
    }
    pthread_mutex_unlock(&mutex);
}
int bio_worker_submit(const BioJob *job,const uint8_t *image,uint16_t w,uint16_t h,const uint8_t *ir)
{
    if(!started||!job||(job->kind!=CREDENTIAL_PALM&&job->kind!=CREDENTIAL_FACE&&job->kind!=CREDENTIAL_FINGER))return -1;
    if((job->kind==CREDENTIAL_PALM&&!palm)||(job->kind==CREDENTIAL_FACE&&!face)||
       (job->kind==CREDENTIAL_FINGER&&!finger))return -1;
    pthread_mutex_lock(&mutex);
    if(state||stop){pthread_mutex_unlock(&mutex);return 0;}
    if(job->stage!=BIO_AUTH) {
        if(!image||!w||!h){pthread_mutex_unlock(&mutex);return -1;}
        if(job->kind==CREDENTIAL_FACE) {
            if(job->require_ir_liveness) {
                if(!ir||!face||!face->live_ir||(w&1)||(h&1)){pthread_mutex_unlock(&mutex);return -1;}
                size_t y=(size_t)w*h;input=malloc(y*3/2);
                if(!input){pthread_mutex_unlock(&mutex);return -1;}
                memcpy(input,ir,y);memset(input+y,128,y/2);
            }
            sample=face?face->sample_capture(image,w,h,&job->face):NULL;
            if(!sample){if(input){memset(input,0,(size_t)w*h*3/2);free(input);input=NULL;}pthread_mutex_unlock(&mutex);return -1;}
        } else {
            input=malloc((size_t)w*h);
            if(!input){pthread_mutex_unlock(&mutex);return -1;}
            memcpy(input,image,(size_t)w*h);
        }
    }
    slot=*job;width=w;height=h;state=1;slot_generation=++generation;
    pthread_cond_signal(&wake);pthread_mutex_unlock(&mutex);return 1;
}
int bio_worker_take(BioJob *result)
{
    pthread_mutex_lock(&mutex);int ready=state==2;
    if(ready){*result=slot;memset(&slot,0,sizeof(slot));state=0;}
    pthread_mutex_unlock(&mutex);return ready;
}
int bio_worker_wait_result(unsigned timeout_ms)
{
    struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);
    deadline.tv_sec+=timeout_ms/1000;
    deadline.tv_nsec+=(long)(timeout_ms%1000)*1000000;
    if(deadline.tv_nsec>=1000000000){++deadline.tv_sec;deadline.tv_nsec-=1000000000;}
    pthread_mutex_lock(&mutex);
    while(!stop&&state!=2) {
        int rc=pthread_cond_timedwait(&result_ready,&mutex,&deadline);
        if(rc)break;
    }
    int ready=!stop&&state==2;
    pthread_mutex_unlock(&mutex);return ready;
}
