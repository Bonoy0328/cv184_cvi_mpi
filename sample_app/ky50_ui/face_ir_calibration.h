#ifndef KY50_FACE_IR_CALIBRATION_H
#define KY50_FACE_IR_CALIBRATION_H
#include <stdint.h>
#include <stddef.h>
#include "pvaFaceRec.h"
#define FACE_IR_CALIBRATION_FRAMES 120
#define FACE_IR_PAIR_MAX_US 50000
typedef struct { uint64_t rgb_pts_us,ir_pts_us; } FaceIrPair;
enum face_ir_calibration_state { FACE_IR_IDLE, FACE_IR_COLLECTING, FACE_IR_DONE, FACE_IR_ERROR };
typedef struct {
    enum face_ir_calibration_state state;
    int count,calibrated;
    float rms_px;
    char message[192];
} FaceIrCalibrationStatus;
#ifdef __cplusplus
extern "C" {
#endif
/* Load an optional device override, otherwise the shared application profile.
 * Missing profiles preserve the original IR detector. Invalid profiles fail
 * closed until a successful recalibration; never silently disable liveness. */
int face_ir_calibration_init(const char *directory,const char *application_profile);
int face_ir_calibration_begin(uint64_t now);
void face_ir_calibration_cancel(void);
void face_ir_calibration_status(uint64_t now,FaceIrCalibrationStatus *out);
int face_ir_calibration_active(void);
int face_ir_pair_valid(const FaceIrPair *pair);
void face_ir_calibration_feed(const FacePoseRes_t *rgb,const PvaFaceGeometry *ir,
    uint16_t width,uint16_t height,const FaceIrPair *pair,uint64_t now);
/* 1 mapped; 0 no calibration (legacy detection); -1 invalid/outside/stale.
 * Reject mapped face overflow. Never clamp or fall back on mapping failure. */
int face_ir_calibration_map(const FacePoseRes_t *rgb,uint16_t width,uint16_t height,
    const FaceIrPair *pair,PvaFaceGeometry *ir);
#ifdef __cplusplus
}
#endif
#endif
