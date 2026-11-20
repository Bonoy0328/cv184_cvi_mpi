#ifndef KY50_FINGERPRINT_IMAGE_H
#define KY50_FINGERPRINT_IMAGE_H
#include <stdint.h>
#include <stddef.h>
#define FINGER_WIDTH 480
#define FINGER_HEIGHT 640
#define FINGER_BYTES (FINGER_WIDTH*FINGER_HEIGHT)
/* Native YUYV Y plane -> complete GRAY8 image, counterclockwise 90 degrees.
 * No crop, resize or inversion. The sensor already supplies the accepted polarity. */
static inline int fingerprint_rotate_yuyv(const uint8_t *src,size_t bytes,unsigned w,unsigned h,unsigned stride,uint8_t *dst,size_t capacity)
{
    if(!src||!dst||!w||!h||w>640||h>480||stride<(size_t)w*2||bytes<(size_t)stride*h||capacity<(size_t)w*h)return -1;
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)dst[(size_t)(w-1-x)*h+y]=src[(size_t)y*stride+2*x];
    return 0;
}
#endif
