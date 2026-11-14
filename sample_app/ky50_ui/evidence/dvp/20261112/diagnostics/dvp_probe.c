/* Isolated GC0328 capture diagnostic. Stop all UI/media owners first. */
#include "gc0328.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_vi.h"
#include "cvi_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig; stopping=1; }
static uint64_t millis(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
int main(int argc,char **argv)
{
    unsigned seconds=argc>1?(unsigned)strtoul(argv[1],NULL,10):60;
    if(!seconds||seconds>600)return 2;
    setvbuf(stdout,NULL,_IOLBF,0);signal(SIGTERM,stop);signal(SIGINT,stop);
    int fd=open("/tmp/ky50-sensor.lock",O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if(fd<0||flock(fd,LOCK_EX|LOCK_NB)) { perror("sensor lock");return 1; }
    int sys=0,vb=0,cam=0,rc=1;unsigned frames=0,misses=0;
    if(CVI_SYS_Init())goto done;
    sys=1;
    VI_VPSS_MODE_S mode={0};
    for(int i=0;i<VI_MAX_PIPE_NUM;i++)mode.aenMode[i]=VI_OFFLINE_VPSS_OFFLINE;
    if(CVI_SYS_SetVIVPSSMode(&mode))goto done;
    VB_CONFIG_S cfg={.u32MaxPoolCnt=1};
    cfg.astCommPool[0].u32BlkSize=COMMON_GetPicBufferSize(640,480,PIXEL_FORMAT_YUYV,DATA_BITWIDTH_8,COMPRESS_MODE_NONE,DEFAULT_ALIGN);
    cfg.astCommPool[0].u32BlkCnt=8;cfg.astCommPool[0].enRemapMode=VB_REMAP_MODE_CACHED;
    if(CVI_VB_SetConfig(&cfg)||CVI_VB_Init())goto done;
    vb=1;cam=1;
    if(gc0328_start())goto done;
    uint64_t begin=millis(),next=begin+1000;
    while(!stopping&&millis()-begin<seconds*1000ULL) {
        VIDEO_FRAME_INFO_S f={0};int result=CVI_VI_GetChnFrame(0,0,&f,100);
        if(!result) {
            frames++;
            if(frames==1) {
                VIDEO_FRAME_S *v=&f.stVFrame;
                printf("FIRST_FRAME %ux%u fmt=%d stride=%u len=%u pts=%llu\n",v->u32Width,v->u32Height,v->enPixelFormat,v->u32Stride[0],v->u32Length[0],(unsigned long long)v->u64PTS);
                void *data=CVI_SYS_MmapCache(v->u64PhyAddr[0],v->u32Length[0]);
                if(data) {
                    if(!CVI_SYS_IonInvalidateCache(v->u64PhyAddr[0],data,v->u32Length[0])) {
                        FILE *out=fopen("/tmp/ky50-gc0328-first.raw","wb");
                        if(out) { fwrite(data,1,v->u32Length[0],out);fclose(out); }
                    }
                    CVI_SYS_Munmap(data,v->u32Length[0]);
                }
            }
            if(CVI_VI_ReleaseChnFrame(0,0,&f))goto done;
        } else misses++;
        if(millis()>=next) {
            printf("DVP_PROBE seconds=%.1f frames=%u misses=%u last_rc=%#x\n",(millis()-begin)/1000.0,frames,misses,result);
            next=millis()+1000;
        }
    }
    rc=frames?0:1;
done:
    if(cam)gc0328_stop();
    if(vb)CVI_VB_Exit();
    if(sys)CVI_SYS_Exit();
    close(fd);printf("DVP_PROBE_DONE frames=%u rc=%d\n",frames,rc);return rc;
}
