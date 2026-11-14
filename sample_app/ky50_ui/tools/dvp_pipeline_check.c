/* Headless integration check: real cameras -> production VPSS/rotation/crop.
 * Stop UI and all other media owners first. Does not initialize LCD or audio. */
#include "media.h"
#include "ui.h"
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig;stopping=1; }
static unsigned long long ms(void)
{
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (unsigned long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
int main(int argc,char **argv)
{
    unsigned cycles=argc>1?(unsigned)strtoul(argv[1],NULL,10):2;
    if(!cycles||cycles>10||argc>2) return 2;
    setvbuf(stdout,NULL,_IOLBF,0);
    signal(SIGINT,stop);signal(SIGTERM,stop);
    int lock=open("/tmp/ky50-sensor.lock",O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB)) { perror("sensor lock");return 1; }
    unsigned char *rgb=malloc(UI_WIDTH*UI_HEIGHT*3);
    int rc=1,camera=0,scale=0;
    if(!rgb||media_system_start()) goto done;
    const enum camera_source seq[]={CAMERA_DVP,CAMERA_RGB,CAMERA_IR,CAMERA_DVP};
    for(unsigned cycle=0;cycle<cycles&&!stopping;cycle++) {
        for(unsigned phase=0;phase<sizeof(seq)/sizeof(seq[0])&&!stopping;phase++) {
            enum camera_source src=seq[phase];camera=1;
            if(media_camera_start(src)) goto done;
            scale=1;if(media_scale_start(src)) goto done;
            unsigned count=0,misses=0;
            unsigned long long begin=ms();
            while(count<60&&!stopping&&ms()-begin<10000) {
                VIDEO_FRAME_INFO_S frame={0};
                if(media_camera_get(&frame)) { misses++;continue; }
                int result=media_scale_frame(&frame,rgb,NULL,src==CAMERA_IR);
                if(media_camera_release(&frame)||result) goto done;
                count++;
                if(src==CAMERA_DVP&&count==40) {
                    char path[96];
                    snprintf(path,sizeof(path),"/tmp/ky50-dvp-scaled-%u-%u.ppm",cycle,phase);
                    FILE *out=fopen(path,"wb");
                    if(!out) goto done;
                    int bad=fprintf(out,"P6\n%d %d\n255\n",UI_WIDTH,UI_HEIGHT)<0;
                    if(fwrite(rgb,1,UI_WIDTH*UI_HEIGHT*3,out)!=UI_WIDTH*UI_HEIGHT*3) bad=1;
                    if(fclose(out)) bad=1;
                    if(bad) goto done;
                }
            }
            printf("PIPELINE cycle=%u phase=%u source=%d frames=%u misses=%u elapsed_ms=%llu\n",
                cycle,phase,src,count,misses,ms()-begin);
            media_scale_stop();scale=0;media_camera_stop();camera=0;
            if(count!=60) goto done;
        }
    }
    rc=stopping?1:0;
done:
    if(scale) media_scale_stop();
    if(camera) media_camera_stop();
    media_system_stop();free(rgb);close(lock);
    printf("PIPELINE_DONE rc=%d\n",rc);return rc;
}
