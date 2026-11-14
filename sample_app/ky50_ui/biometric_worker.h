#ifndef KY50_BIOMETRIC_WORKER_H
#define KY50_BIOMETRIC_WORKER_H
#include "access.h"
enum bio_stage { BIO_FEATURE, BIO_AUTH, BIO_FACE_RECOGNIZE, BIO_PALM_RECOGNIZE };
typedef struct {
    unsigned kind;
    enum bio_stage stage;
    HandPoseRes_t hand;
    FacePoseRes_t face;
    uint8_t feature[APP_FEATURE_BYTES];
    User_For_C_t user;
    int result,auth_result,rgb_preview;
    uint64_t submitted_ms;
    IrExposureSample ir_sample;
    FaceIrPair face_pair;
    IrExposureProof ir_proof;
    int auth_ran;
    int require_ir_liveness,ir_live_result;
    uint64_t ir_live_us,completed_us;
} BioJob;
int bio_worker_start(const PalmBackend *palm,const FaceBackend *face);
void bio_worker_stop(void);
int bio_worker_busy(void);
/* Submit/take are serialized by the application's state lock. No backlog.
 * Waiting does not own the result; callers recheck eligibility before take. */
int bio_worker_wait_result(unsigned timeout_ms);
int bio_worker_submit(const BioJob *job,const uint8_t *image,uint16_t w,uint16_t h,const uint8_t *ir);
int bio_worker_take(BioJob *result);
void bio_worker_cancel(unsigned kind); /* 0 = either modality */
int bio_face_live_ir(const FaceBackend *backend,uint8_t *ir,uint16_t w,uint16_t h,
                     const FacePoseRes_t *pose,const FaceIrPair *pair);
#endif
