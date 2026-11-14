/* Reuse the verified panel transport, timing, CRC workaround and three VB
 * blocks. Never overwrite a buffer after submitting it to VO.
 */
#include "../ky50_sensor/display.c"
#include "media.h"
#include "pixels.h"
static const char *snapshot_path;
void media_display_snapshot(const char *path) { snapshot_path=path; }
int media_display_start(void) { return camera_display_start(); }
int media_display_present(const unsigned char *rgb,const uint32_t *argb)
{
    if(camera_display_begin()) return -1;
    for(unsigned y=0;y<HEIGHT;y++) {
        unsigned char *dst=canvas+y*OUT_STRIDE;
        const unsigned char *src=rgb+y*WIDTH*3;
        const uint32_t *ui=argb+y*WIDTH;
        pixels_blend(dst,src,ui,WIDTH);
    }
    if(snapshot_path) {
        FILE *f=fopen(snapshot_path,"wb");
        int ok=f!=NULL;
        if(f) {
            if(fprintf(f,"P6\n480 854\n255\n")<0) ok=0;
            for(unsigned y=0;y<HEIGHT;y++)
                if(fwrite(canvas+y*OUT_STRIDE,3,WIDTH,f)!=WIDTH) ok=0;
            if(fclose(f)) ok=0;
        }
        printf("SNAPSHOT %s %s\n",snapshot_path,ok?"OK":"FAILED");snapshot_path=NULL;
    }
    return camera_display_end();
}
void media_display_stop(void) { camera_display_stop(); }
