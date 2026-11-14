#ifndef KY50_SENSOR_DISPLAY_H
#define KY50_SENSOR_DISPLAY_H
#include "cvi_comm_video.h"
int camera_display_start(void);
int camera_display_begin(void);
int camera_display_add(const VIDEO_FRAME_INFO_S *frame, int rgb);
int camera_display_end(void);
void camera_display_stop(void);
#endif
