#ifndef KY50_ACCESS_STORE_H
#define KY50_ACCESS_STORE_H
#include <stdint.h>
#include <stddef.h>
#include "sqlite3.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Local profile capacity is independent of either SDK ID namespace. */
#define APP_MAX_USERS 100
#define APP_TEMPLATE_SLOTS 100
#define APP_NAME_BYTES 129
#define APP_EMPLOYEE_BYTES 33
#define APP_AVATAR_BYTES 256
#define CREDENTIAL_FACE 1u
#define CREDENTIAL_PALM 2u
#define CREDENTIAL_FINGER 4u
#define CREDENTIAL_CARD 8u
#define CREDENTIAL_PASSWORD 16u
#define APP_FINGER_MODEL_ID "cv184x-fingerprint-embed_p333-roi-20261116"
enum credential_state { CRED_NONE, CRED_PENDING, CRED_REGISTERED, CRED_MISSING };
typedef struct {
    int64_t record_id;
    uint16_t local_id;
    char employee_id[APP_EMPLOYEE_BYTES],name[APP_NAME_BYTES],avatar[APP_AVATAR_BYTES];
    unsigned registered;
    int enabled,is_admin,deleting,palm_state,face_state,finger_state;
    int palm_uuid,face_uuid,finger_uuid; /* -1: no template; each SDK namespace is independent. */
} AppUser;
typedef struct { sqlite3 *db; char error[192]; } UserStore;
typedef struct { uint16_t local_id; int deleting; unsigned modality; int template_uuid; } PendingOperation;
/* Thresholds in hundredths, matching the SDK configuration precision. */
typedef struct { int hand_threshold,auth_threshold,dynamic_level; } PalmParameters;
/* ModelProbabilityThresholdFace and FaceThreshold are the same SDK setting. */
typedef struct {
    int detection_threshold,quality_threshold,liveness_threshold;
    int quality_enable,liveness_enable,auth_threshold,filter_threshold;
} FaceParameters;
int face_parameters_valid(const FaceParameters *p);
int store_face_parameters_load(UserStore *s,FaceParameters *p);
int store_face_parameters_save(UserStore *s,const FaceParameters *p);
int palm_parameters_valid(const PalmParameters *p);
/* 0 loaded, 1 no saved override, -1 invalid/unreadable. */
int store_parameters_load(UserStore *s,PalmParameters *p);
int store_parameters_save(UserStore *s,const PalmParameters *p);
int store_ir_target_load(UserStore *s,int *target);
int store_ir_target_save(UserStore *s,int target);
int store_open(UserStore *s,const char *path);
void store_close(UserStore *s);
/* Passive maintenance while idle; truncate only at controlled shutdown.
 * SQLITE_BUSY is retried later. All calls share the application's state lock. */
int store_checkpoint(UserStore *s,int truncate);
int store_list(UserStore *s,AppUser *users,int capacity);
int store_get(UserStore *s,uint16_t local_id,AppUser *user);
int store_create(UserStore *s,const char *employee,const char *name,AppUser *out);
/* Query using (modality, SDK template UUID), never the local profile ID. */
int store_get_by_template(UserStore *s,unsigned kind,uint16_t template_uuid,AppUser *user);
int store_templates_reset_pending(UserStore *s);
int store_finish_templates_reset(UserStore *s);
int store_fingerprint_reset_pending(UserStore *s);
int store_finish_fingerprint_reset(UserStore *s);
/* Modality-aware operations; wrappers below remain palm-only. */
int store_prepare_enroll_kind(UserStore *s,uint16_t local_id,unsigned kind,uint16_t template_uuid);
int store_finish_enroll_kind(UserStore *s,uint16_t local_id,int side,unsigned kind);
int store_cancel_enroll_kind(UserStore *s,uint16_t local_id,const char *reason,unsigned kind);
int store_mark_missing_kind(UserStore *s,uint16_t local_id,unsigned kind);
int store_record_match_kind(UserStore *s,const AppUser *user,float score,uint64_t ms,unsigned kind);
int store_record_unknown_kind(UserStore *s,uint64_t ms,unsigned kind);
int store_prepare_enroll(UserStore *s,uint16_t local_id,uint16_t template_uuid);
int store_finish_enroll(UserStore *s,uint16_t local_id,int hand_side);
int store_cancel_enroll(UserStore *s,uint16_t local_id,const char *reason);
int store_prepare_delete(UserStore *s,uint16_t local_id);
int store_finish_delete(UserStore *s,uint16_t local_id);
int store_pending(UserStore *s,PendingOperation *ops,int capacity);
int store_mark_missing(UserStore *s,uint16_t local_id);
int store_record_match(UserStore *s,const AppUser *user,float score,uint64_t monotonic_ms);
int store_record_unknown(UserStore *s,uint64_t monotonic_ms);
int store_employee_valid(const char *text);
int store_name_valid(const char *text);
void app_utf8_copy(char *out,size_t capacity,const char *in);
#ifdef __cplusplus
}
#endif
#endif
