#ifndef KY50_MEDIA_H
#define KY50_MEDIA_H
#include <stdint.h>
#include "cvi_comm_video.h"
#include "settings.h"
#include "hand_overlay.h"
#include "ir_exposure.h"
extern const IrExposureBackend media_ir_exposure_backend;
int media_system_start(void);
void media_system_stop(void);
int media_camera_start(enum camera_source source);
int media_camera_select_preview(enum camera_source source);
int media_camera_get(VIDEO_FRAME_INFO_S *frame);
int media_camera_release(VIDEO_FRAME_INFO_S *frame);
int media_camera_get_ir(VIDEO_FRAME_INFO_S *frame);
int media_ir_sample(const VIDEO_FRAME_INFO_S *frame,IrExposureSample *sample);
int media_camera_release_ir(VIDEO_FRAME_INFO_S *frame);
int media_camera_get_rgb(VIDEO_FRAME_INFO_S *frame);
int media_camera_release_rgb(VIDEO_FRAME_INFO_S *frame);
void media_camera_stop(void);
int media_scale_start(enum camera_source source);
int media_scale_frame(const VIDEO_FRAME_INFO_S *input,unsigned char *rgb,HandViewport *view,int monochrome);
int media_ir_half_frame(const VIDEO_FRAME_INFO_S *input,unsigned char *gray);
int media_rgb_half_frame(const VIDEO_FRAME_INFO_S *input,unsigned char *nv12);
void media_scale_stop(void);
int media_face_color_nv12(const uint8_t *nv12,uint16_t width,uint16_t height,uint8_t *bgr,uint32_t stride);
void media_face_color_stop(void);
int media_display_start(void);
int media_display_present(const unsigned char *rgb,const uint32_t *argb);
void media_display_stop(void);
void media_display_snapshot(const char *path);
#endif
