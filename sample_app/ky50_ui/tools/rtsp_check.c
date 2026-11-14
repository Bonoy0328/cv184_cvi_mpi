/* Standalone real-camera integration check. Stop ky50_ui before running.
 * No models, personal database, display, or synthetic frames are used. */
#include "rtsp_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/file.h>
static unsigned long long us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000+t.tv_nsec/1000;}
static volatile sig_atomic_t stop;
static void on_signal(int n){(void)n;stop=1;}
int main(int argc,char **argv)
{
    int seconds=argc>1?atoi(argv[1]):30,rc=1;
    int lock=open("/tmp/ky50-sensor.lock",O_CREAT|O_RDWR,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){perror("media lock");return 2;}
    setvbuf(stdout,NULL,_IOLBF,0);signal(SIGINT,on_signal);signal(SIGTERM,on_signal);
    if(media_system_start())goto done;
    if(media_camera_start_mode(CAMERA_RGB,1))goto done;
    if(rtsp_stream_start())goto done;
    time_t end=time(NULL)+seconds,log=0;
    unsigned frames[3]={0};
    while(!stop&&time(NULL)<end) {
        for(int i=0;i<3;i++) {
            unsigned long long t0=us(),t1,t2;
            VIDEO_FRAME_INFO_S f={0};
            int got=i==CAMERA_RGB?media_camera_get_rgb(&f):i==CAMERA_IR?media_camera_get_ir(&f):media_camera_get_dvp(&f);
            t1=us();
            if(!got) {
                rtsp_stream_send(i,&f);t2=us();frames[i]++;
                if(i==CAMERA_RGB)media_camera_release_rgb(&f);
                else if(i==CAMERA_IR)media_camera_release_ir(&f);
                else media_camera_release_dvp(&f);
                if(frames[i]<=5)printf("STAGE ch=%d get=%llu send=%llu release=%llu\n",i,t1-t0,t2-t1,us()-t2);
            }
        }
        if(time(NULL)>=log){printf("TRIPLE rgb=%u ir=%u dvp=%u health=%d\n",frames[0],frames[1],frames[2],rtsp_stream_health());log=time(NULL)+5;}
        if(rtsp_stream_health()<0)goto done;
    }
    rc=frames[0]&&frames[1]&&frames[2]?0:1;
done:
    rtsp_stream_stop();media_camera_stop();media_system_stop();close(lock);
    printf("RTSP_CHECK rc=%d\n",rc);return rc;
}
