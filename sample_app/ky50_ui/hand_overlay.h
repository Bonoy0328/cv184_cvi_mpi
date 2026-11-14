#ifndef KY50_HAND_OVERLAY_H
#define KY50_HAND_OVERLAY_H
#include <stdint.h>
#include <stddef.h>
#include "pvaTypeDef.h"
#include "pvaFaceRec.h"
#define HAND_INPUT_WIDTH 540
#define HAND_INPUT_HEIGHT 960
#define HAND_COLOR_OK 0x36ff70u
#define HAND_COLOR_INVALID 0xff4848u
typedef struct { unsigned input_width,input_height,scaled_width,scaled_height; int crop_x,crop_y; } HandViewport;
typedef struct { int left,top,right,bottom; int points[14]; int status; } HandOverlay;
#define HAND_DRAW_MISS_FRAMES 3
#define HAND_DRAW_MISS_MS 150
#define HAND_DRAW_RESULT_MS 500
typedef struct {
    HandOverlay drawing;
    uint64_t updated_ms,miss_started_ms;
    unsigned missing;
    int valid;
} HandOverlayState;
#ifdef __cplusplus
extern "C" {
#endif
/* Display-only debounce. Never feeds back into detection, AE or access. */
void hand_overlay_reset(HandOverlayState *state);
void hand_overlay_detected(HandOverlayState *state);
void hand_overlay_update(HandOverlayState *state,const HandOverlay *drawing,int found,uint64_t now);
int hand_overlay_snapshot(const HandOverlayState *state,uint64_t now,HandOverlay *drawing);
/* Copy only Y, removing allocation padding without cropping image content. */
int hand_copy_y(uint8_t *dst,unsigned dw,unsigned dh,const uint8_t *src,
                unsigned sw,unsigned sh,unsigned stride,unsigned x,unsigned y);
/* Rotate a full plane CCW90. unit=1 for Y, 2 for an interleaved VU pair.
 * Destination is tightly packed; source and destination must not overlap. */
int hand_rotate_ccw90(uint8_t *dst,const uint8_t *src,unsigned width,
                     unsigned height,unsigned stride,unsigned unit);
/* RGB888 in screen coordinates. Clips boxes and markers to the image bounds. */
int hand_map(const HandPoseRes_t *copy,const HandViewport *view,HandOverlay *drawing);
/* Recognition green requires accepted business success; enrollment shows quality. */
int hand_overlay_display_status(int quality,int confirmed,int enrolling);
void hand_draw(uint8_t *rgb,unsigned width,unsigned height,const HandOverlay *drawing);
int handpose_start(const char *config);
int handpose_detect(uint8_t *yuv,uint16_t width,uint16_t height,HandPoseRes_t *pose);
void handpose_stop(void);
int facepose_enabled(void);
int facepose_detect_ir(const uint8_t *nv12,uint16_t width,uint16_t height,PvaFaceGeometry *geometry);
int facepose_detect(uint8_t *nv12,uint16_t width,uint16_t height,FacePoseRes_t *pose);
int face_map(const FacePoseRes_t *copy,const HandViewport *view,HandOverlay *drawing);
/* status=-1 denotes detection only (blue); it is not an authentication result. */
void face_draw(uint8_t *rgb,unsigned width,unsigned height,const HandOverlay *drawing);
int color_nv21_ccw90(uint8_t *nv12,const uint8_t *y,const uint8_t *vu,
                     unsigned width,unsigned height,unsigned y_stride,unsigned vu_stride);
/* Hardware-rotated NV21: remove padding and change VU pairs to NV12 UV. */
int color_nv21_copy(uint8_t *nv12,const uint8_t *y,const uint8_t *vu,
                     unsigned width,unsigned height,unsigned y_stride,unsigned vu_stride);
#ifdef __cplusplus
}
#endif
#endif
