#ifndef KY50_RTSP_STREAM_H
#define KY50_RTSP_STREAM_H
#include "media.h"
/* Capture thread owns lifecycle and SendFrame. An independent worker drains
 * VENC and writes RTSP; it never calls LVGL or acquires VI frames. */
int rtsp_stream_start(void);
void rtsp_stream_stop(void);
void rtsp_stream_send(enum camera_source source,const VIDEO_FRAME_INFO_S *frame);
/* 1: all three encoders have emitted a frame; 0: starting; -1: stalled. */
int rtsp_stream_health(void);
#endif
