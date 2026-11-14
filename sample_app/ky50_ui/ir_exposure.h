#ifndef KY50_IR_EXPOSURE_H
#define KY50_IR_EXPOSURE_H
#include <stdint.h>
typedef struct {
    int target;
    unsigned time_us,again,dgain,isp_gain,luma;
    int stable;
    uint64_t sequence;
} IrExposureReading;
/* Metadata belongs to the IR image, not the later detection/result time. */
typedef struct {
    uint64_t sequence,captured_ms;
    int target,stable,valid;
} IrExposureSample;
typedef struct {
    uint64_t generation,sequence;
    int target;
} IrExposureProof;
typedef struct {
    int (*get)(IrExposureReading *reading);
    int (*set)(int target);
} IrExposureBackend;
typedef struct {
    const IrExposureBackend *backend;
    int saved_target,active,direction,last_direction,repeats,step;
    uint64_t next_adjust,last_change,generation,change_sequence,last_feedback_sequence;
    int target,observe_pending;
} IrExposure;
void ir_exposure_init(IrExposure *s,const IrExposureBackend *backend,int saved_target);
/* Reset after confirmed departure/camera restart; never accesses the ISP.
 * Detection frame counting belongs to the access service, not an AE timeout. */
void ir_exposure_reset(IrExposure *s);
/* Nonzero: feedback was deferred or changed exposure; NEVER an auth gate.
 * SDK quality alone decides whether the image can be matched. No ISP access
 * if found <= 0. Only post-pvaGetFeat status drives brightness feedback. */
int ir_exposure_feedback(IrExposure *s,int found,int status,uint64_t ms,const IrExposureSample *sample);
IrExposureProof ir_exposure_proof(const IrExposure *s,const IrExposureSample *sample);
/* Async completion validates the sample's exposure generation, not equal timestamps. */
int ir_exposure_success(IrExposure *s,const IrExposureProof *proof,uint64_t ms);
#endif
