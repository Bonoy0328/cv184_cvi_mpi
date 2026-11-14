#ifndef KY50_PIXELS_H
#define KY50_PIXELS_H
#include <stdint.h>
/* Count must be a multiple of 8; NV21 input contains count Y and VU bytes. */
void pixels_nv21(uint8_t *rgb,const uint8_t *y,const uint8_t *vu,unsigned count);
/* IR preview: BT.601 luma range, R=G=B regardless of transient ISP chroma. */
void pixels_gray(uint8_t *rgb,const uint8_t *y,unsigned count);
void pixels_blend(uint8_t *dst,const uint8_t *rgb,const uint32_t *argb,unsigned count);
#endif
