#include "ir_exposure.h"
#include "pvaTypeDef.h"
#include <stdio.h>
#include <string.h>
void ir_exposure_reset(IrExposure *s)
{
    s->active=s->direction=s->last_direction=s->repeats=s->observe_pending=0;s->step=4;
    s->next_adjust=s->last_change=s->change_sequence=s->last_feedback_sequence=0;
    s->target=-1;++s->generation;
}
void ir_exposure_init(IrExposure *s,const IrExposureBackend *backend,int saved_target)
{
    memset(s,0,sizeof(*s));s->backend=backend;
    s->saved_target=saved_target>=1&&saved_target<=255?saved_target:-1;
    ir_exposure_reset(s);
}
static int fresh(const IrExposure *s,const IrExposureSample *sample)
{
    return sample&&sample->valid&&sample->target==s->target&&
        (!s->last_change||(sample->captured_ms>s->last_change&&
        sample->sequence>s->change_sequence));
}
IrExposureProof ir_exposure_proof(const IrExposure *s,const IrExposureSample *sample)
{
    return (IrExposureProof){.generation=s->generation,.sequence=sample&&sample->valid?sample->sequence:0,
        .target=sample?sample->target:-1};
}
static int change(IrExposure *s,int old,int target,const char *reason,int status,uint64_t ms)
{
    /* Snapshot the current capture counter AFTER the write, not the old image's
     * counter. The capture thread can advance while feature extraction runs. */
    IrExposureReading r;
    ++s->generation;s->observe_pending=1;s->target=target;
    if(s->backend->set(target)||s->backend->get(&r)) {
        fprintf(stderr,"IR_AE set/read failed reason=%s target=%d\n",reason,target);
        s->active=0;s->next_adjust=ms+1000;return -1;
    }
    s->last_change=ms;s->change_sequence=r.sequence;s->next_adjust=0;
    printf("IR_AE found=1 status=%d reason=%s target=%d->%d auth_wait=0 base_sequence=%llu uptime_ms=%llu\n",
        status,reason,old,target,(unsigned long long)r.sequence,(unsigned long long)ms);
    return 0;
}
int ir_exposure_feedback(IrExposure *s,int found,int status,uint64_t ms,const IrExposureSample *sample)
{
    if(!s->backend)return 0;
    if(found<=0) {s->direction=s->repeats=0;return 0;}
    if(ms<s->next_adjust)return 1;
    if(!s->active) {
        IrExposureReading r;
        if(s->backend->get(&r)) {s->next_adjust=ms+1000;return 1;}
        s->target=r.target;
        /* Do not disturb an already usable image just to recall a historical
         * target. Recall is an aid for poor images, never a prerequisite. */
        if(status!=STATUS_OK&&s->saved_target>=1&&r.target!=s->saved_target) {
            if(change(s,r.target,s->saved_target,"restore_success",status,ms))return 1;
            s->active=1;s->direction=s->last_direction=s->repeats=0;s->step=4;return 1;
        }
        printf("IR_AE found=1 resume_success target=%d unchanged\n",r.target);
        s->active=1;s->direction=s->last_direction=s->repeats=0;s->step=4;
    }
    /* Old/duplicate exposure feedback cannot adjust today's ISP target.
     * Authentication independently uses the SDK quality of its owned image. */
    if(!fresh(s,sample)||sample->sequence<=s->last_feedback_sequence)return 1;
    s->last_feedback_sequence=sample->sequence;
    if(s->observe_pending) {
        s->observe_pending=0;
        printf("IR_AE observed frames=%llu capture_age_ms=%llu feedback_age_ms=%llu stable=%d target=%d auth_wait=0\n",
            (unsigned long long)(sample->sequence-s->change_sequence),
            (unsigned long long)(sample->captured_ms-s->last_change),
            (unsigned long long)(ms-s->last_change),sample->stable,s->target);
    }
    int direction=status==STATUS_TOO_BRIGHT?-1:status==STATUS_TOO_DARK?1:0;
    if(!direction){s->direction=s->repeats=0;return 0;}
    if(direction!=s->direction){s->direction=direction;s->repeats=1;return 0;}
    if(++s->repeats<2)return 0;
    s->repeats=0;
    IrExposureReading r;
    if(s->backend->get(&r)) {s->next_adjust=ms+1000;return 1;}
    if(s->last_direction&&s->last_direction!=direction&&s->step>1)s->step/=2;
    s->last_direction=direction;
    int target=r.target+direction*s->step;
    if(target<1)target=1;
    if(target>255)target=255;
    if(target==r.target) {
        printf("IR_AE found=1 status=%d limit=%d luma=%u time_us=%u\n",status,target,r.luma,r.time_us);
        s->next_adjust=ms+2000;return 0;
    }
    printf("IR_AE measured target=%d luma=%u time_us=%u gain=%u/%u/%u stable=%d\n",
        r.target,r.luma,r.time_us,r.again,r.dgain,r.isp_gain,r.stable);
    change(s,r.target,target,direction<0?"too_bright":"too_dark",status,ms);
    return 1;
}
int ir_exposure_success(IrExposure *s,const IrExposureProof *proof,uint64_t ms)
{
    if(!s->backend||!s->active||!proof||!proof->sequence||
       proof->generation!=s->generation||proof->target!=s->target||ms<s->next_adjust)return -1;
    IrExposureReading r;
    if(s->backend->get(&r)||r.target!=proof->target||r.target<1||r.target>255)return -1;
    s->saved_target=proof->target;
    printf("IR_AE recognition_success target=%d sample_sequence=%llu generation=%llu luma=%u time_us=%u gain=%u/%u/%u\n",
        r.target,(unsigned long long)proof->sequence,(unsigned long long)proof->generation,
        r.luma,r.time_us,r.again,r.dgain,r.isp_gain);
    return r.target;
}
