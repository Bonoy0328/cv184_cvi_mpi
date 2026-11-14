#ifndef KY50_ACCESS_H
#define KY50_ACCESS_H
#include "access_store.h"
#include "pvaTypeDef.h"
#include "ir_exposure.h"
#include "app_config.h"
#include "face_ir_calibration.h"
#define APP_FEATURE_BYTES 1024
#define APP_TEMPLATE_BYTES (1024*5)
#define PALM_ENTER_FRAMES 2
typedef struct {
    int (*ids)(uint16_t *ids,int capacity);
    int (*exists)(uint16_t uuid);
    int (*complete)(uint16_t uuid);
    int (*remove)(uint16_t uuid);
    int (*clear)(void);
    int (*capacity)(void);
    int (*features)(uint8_t *yuv,uint16_t w,uint16_t h,HandPoseRes_t *pose,uint8_t *feat);
    int (*enroll)(User_For_C_t *user,uint8_t *feat,ResgisRes_t *progress);
    int (*recognize)(uint8_t *feat,User_For_C_t *user);
    int (*parameters_get)(PalmParameters *parameters);
    int (*parameters_set)(const PalmParameters *parameters);
    int (*pick_free)(void); /* SDK query only; registration persists the binding. */
} PalmBackend;
typedef struct {
    int (*ids)(uint16_t *ids,int capacity);
    int (*exists)(uint16_t uuid);
    int (*complete)(uint16_t uuid);
    int (*remove)(uint16_t uuid);
    int (*clear)(void);
    int (*capacity)(void);
    int (*features)(uint8_t *nv12,uint16_t w,uint16_t h,FacePoseRes_t *pose,uint8_t *feat);
    int (*enroll)(User_For_C_t *user,uint8_t *feat,float quality,ResgisRes_t *progress);
    int (*recognize)(uint8_t *feat,User_For_C_t *user);
    int (*pick_free)(void);
    int (*parameters_get)(FaceParameters *parameters);
    int (*parameters_set)(const FaceParameters *parameters);
    void *(*sample_capture)(const uint8_t *nv12,uint16_t w,uint16_t h,const FacePoseRes_t *pose);
    int (*sample_features)(const void *sample,FacePoseRes_t *pose,uint8_t *feat);
    void (*sample_free)(void *sample);
    int (*live_ir)(uint8_t *ir_nv12,uint16_t w,uint16_t h);
    int (*live_ir_region)(const uint8_t *ir_nv12,uint16_t w,uint16_t h,const PvaFaceGeometry *region);
} FaceBackend;
enum access_command_type { ACCESS_CREATE, ACCESS_ENROLL, ACCESS_CANCEL, ACCESS_DELETE, ACCESS_PARAMETERS, ACCESS_FACE_PARAMETERS, ACCESS_APP_PARAMETERS };
typedef struct {
    enum access_command_type type;
    uint16_t local_id,ids[APP_MAX_USERS]; /* Application profile IDs, never SDK UUIDs. */int count;
    char employee_id[APP_EMPLOYEE_BYTES],name[APP_NAME_BYTES];
    PalmParameters parameters;
    FaceParameters face_parameters;
    AppParameters app_parameters;
    unsigned modality; /* 0 preserves the existing palm command default. */
} AccessCommand;
enum access_event { ACCESS_EVENT_NONE, ACCESS_EVENT_CREATED, ACCESS_EVENT_ENROLLED, ACCESS_EVENT_DELETED, ACCESS_EVENT_ERROR };
typedef struct AccessSnapshot {
    unsigned revision,event_sequence;
    /* Confirmed recognition only: stable across unrelated UI/config updates. */
    unsigned success_sequence,success_modality,verified_modalities;
    int success_relay_seconds;
    uint64_t success_ms;
    enum access_event event;
    uint16_t selected_id;
    int ready,palm_ready,count,busy,registering,progress,total,status,matched,orphan_count;
    int face_ready;
    unsigned enrollment_modality;
    float score;
    char message[192],recognition[192];
    PalmParameters parameters;
    unsigned parameters_sequence;
    int parameters_ready,parameters_result;
    char parameters_message[128];
    FaceParameters face_parameters;
    unsigned face_parameters_sequence;
    int face_parameters_ready,face_parameters_result;
    char face_parameters_message[128];
    AppParameters app_parameters;
    unsigned app_parameters_sequence;
    int app_parameters_ready,app_parameters_result;
    char app_parameters_message[128];
    AppUser users[APP_MAX_USERS];
} AccessSnapshot;
#ifdef __cplusplus
extern "C" {
#endif
extern const PalmBackend ky50_palm_backend;
extern const FaceBackend ky50_face_backend;
/* UI thread: bounded queue and copies only. */
int access_submit(const AccessCommand *command);
void access_enable_recognition(int enabled);
int access_snapshot(AccessSnapshot *snapshot,unsigned previous_revision);
/* All state/SQLite/ISP entry points below require one application state lock.
 * Detection and result-consumer threads share that lock; SDK Pose inference
 * on owned images runs outside it. UI uses only the queue/snapshot API above. */
int access_open(const char *directory,const PalmBackend *backend);
int access_open_biometrics(const char *directory,const PalmBackend *backend,const FaceBackend *face_backend);
void access_close(void);
int access_start_async(void);
void access_set_exposure_backend(const IrExposureBackend *backend);
/* State-owner thread: set metadata for the next IR image before access_frame
 * or access_biometrics_frame. Metadata is copied into each background job. */
void access_set_ir_sample(const IrExposureSample *sample);
void access_set_face_pair(const FaceIrPair *pair);
void access_exposure_reset(void);
void access_poll(uint64_t monotonic_ms);
/* Returns 1 when this raw detection is confirmed for drawing, 0 otherwise.
 * Success holds auth calls until the configured palm miss count; features
 * continue updating quality and exposure while present. Async mode samples
 * only when the single background job slot is available.
 * Preserve detection coordinates before this call; copy only status back. */
int access_frame(uint8_t *yuv,uint16_t width,uint16_t height,HandPoseRes_t *pose,int found,int rgb_preview,uint64_t monotonic_ms);
/* Face work is allowed only after a valid, raw no-hand result. A detected
 * hand takes priority even before entry confirmation or if its quality fails.
 * Pose calls and sample submission run on the detection thread. Background results
 * are accepted only while their original modality/session remains eligible. */
void access_biometrics_frame(uint8_t *ir,uint8_t *rgb_nv12,uint16_t w,uint16_t h,
                            HandPoseRes_t *hand,int hand_found,FacePoseRes_t *face,int face_found,
                            int rgb_preview,uint64_t ms);
/* Announce raw palm priority before starting the RGB Pose call. Does not
 * advance departure/entry counters. Results never borrow detector images. */
void access_observe_hand(int hand_found,uint64_t ms);
int access_consume_results(uint64_t ms);
void access_idle_maintenance(uint64_t ms);
/* A frame/API error breaks consecutive counts, but never counts as departure. */
void access_detection_gap(void);
void access_log_pipeline(void);
/* Base directory used to validate any configured user avatar path. */
const char *access_data_directory(void);
#ifdef __cplusplus
}
#endif
#endif
