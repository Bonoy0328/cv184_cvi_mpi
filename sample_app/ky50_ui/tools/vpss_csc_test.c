/* Board-only synthetic input; never captures or saves camera images.
 * Stop the media application first. Uses its exact preview scaling path. */
#include "media.h"
#include "ui.h"
#include "pixels.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_vpss.h"
#include "cvi_gdc.h"
#include "cvi_buffer.h"
#include <sys/file.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(call) do{int error=(call);if(error){fprintf(stderr,"%s: %#x\n",#call,error);goto done;}}while(0)
static double ms(clockid_t id){struct timespec t;clock_gettime(id,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static int rotation_check(const VIDEO_FRAME_INFO_S *input)
{
    int rc=1,attached=0;VB_POOL pool=VB_INVALID_POOLID;
    MMF_CHN_S channel={.enModId=CVI_ID_VPSS,.s32DevId=0,.s32ChnId=0};
    VB_POOL_CONFIG_S config={.u32BlkCnt=3,.enRemapMode=VB_REMAP_MODE_CACHED};
    config.u32BlkSize=COMMON_GetPicBufferSize(512,896,PIXEL_FORMAT_NV21,DATA_BITWIDTH_8,COMPRESS_MODE_NONE,DEFAULT_ALIGN);
    pool=CVI_VB_CreatePool(&config);if(pool==VB_INVALID_POOLID)goto done;
    CHECK(CVI_GDC_AttachVbPool(&channel,pool));attached=1;
    CHECK(CVI_VPSS_SetChnRotation(0,0,ROTATION_270));
    double wall=ms(CLOCK_MONOTONIC),cpu=ms(CLOCK_PROCESS_CPUTIME_ID);
    for(int i=0;i<300;i++) {
        VIDEO_FRAME_INFO_S output={0};CHECK(CVI_VPSS_SendFrame(0,input,300));
        CHECK(CVI_VPSS_GetChnFrame(0,0,&output,500));
        VIDEO_FRAME_S *v=&output.stVFrame;
        int width=v->u32Width-v->s16OffsetLeft-v->s16OffsetRight;
        int height=v->u32Height-v->s16OffsetTop-v->s16OffsetBottom;
        if(i==0)printf("GDC_OUTPUT width=%u height=%u stride=%u offsets=%d,%d,%d,%d effective=%dx%d\n",
            v->u32Width,v->u32Height,v->u32Stride[0],v->s16OffsetLeft,v->s16OffsetRight,v->s16OffsetTop,v->s16OffsetBottom,width,height);
        int ok=width==486&&height==864;
        if(ok) {
            uint8_t *y=CVI_SYS_MmapCache(v->u64PhyAddr[0],v->u32Length[0]);
            if(!y||y==MAP_FAILED)ok=0;
            else {
                if(CVI_SYS_IonInvalidateCache(v->u64PhyAddr[0],y,v->u32Length[0]))ok=0;
                const uint8_t expected[]={145,235,82,41};int n=0;
                for(int yy=100;yy<=700;yy+=600)for(int x=100;x<=380;x+=280) {
                    int value=y[(yy+v->s16OffsetTop)*v->u32Stride[0]+x+v->s16OffsetLeft];
                    if(abs(value-expected[n++])>1)ok=0;
                }
                CVI_SYS_Munmap(y,v->u32Length[0]);
            }
        }
        CHECK(CVI_VPSS_ReleaseChnFrame(0,0,&output));
        if(!ok){fprintf(stderr,"GDC_BAD frame=%d effective=%dx%d\n",i,width,height);goto done;}
    }
    printf("PASS GDC CCW90 300 frames wall_ms=%.3f cpu_ms=%.3f dedicated_pool=%u\n",(ms(CLOCK_MONOTONIC)-wall)/300,(ms(CLOCK_PROCESS_CPUTIME_ID)-cpu)/300,pool);rc=0;
done:
    CVI_VPSS_SetChnRotation(0,0,ROTATION_0);
    if(attached)CVI_GDC_DetachVbPool(&channel);
    if(pool!=VB_INVALID_POOLID)CVI_VB_DestroyPool(pool);
    return rc;
}
int main(int argc,char **argv)
{
    int rc=1,system=0,vb=0,scaled=0;
    int lock=open("/tmp/ky50-sensor.lock",O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){perror("stop media application first");return 1;}
    VB_BLK block=VB_INVALID_HANDLE;uint8_t *input=NULL,*rgb=malloc(UI_WIDTH*UI_HEIGHT*3);
    VB_CAL_CONFIG_S calc;COMMON_GetPicBufferConfig(1920,1080,PIXEL_FORMAT_NV21,DATA_BITWIDTH_8,COMPRESS_MODE_NONE,DEFAULT_ALIGN,&calc);
    CHECK(CVI_SYS_Init());system=1;
    VI_VPSS_MODE_S mode={0};for(int i=0;i<VI_MAX_PIPE_NUM;i++)mode.aenMode[i]=VI_OFFLINE_VPSS_OFFLINE;
    CHECK(CVI_SYS_SetVIVPSSMode(&mode));
    VB_CONFIG_S config={.u32MaxPoolCnt=1};
    config.astCommPool[0]=(VB_POOL_CONFIG_S){.u32BlkSize=calc.u32VBSize,.u32BlkCnt=1,.enRemapMode=VB_REMAP_MODE_CACHED};
    CHECK(CVI_VB_SetConfig(&config));CHECK(CVI_VB_Init());vb=1;
    block=CVI_VB_GetBlock(VB_INVALID_POOLID,calc.u32VBSize);if(block==VB_INVALID_HANDLE||!rgb)goto done;
    VIDEO_FRAME_INFO_S frame={.u32PoolId=CVI_VB_Handle2PoolId(block)};
    VIDEO_FRAME_S *v=&frame.stVFrame;
    v->u32Width=1920;v->u32Height=1080;v->enPixelFormat=PIXEL_FORMAT_NV21;
    v->u32Stride[0]=calc.u32MainStride;v->u32Stride[1]=calc.u32CStride;
    v->u32Length[0]=calc.u32MainYSize;v->u32Length[1]=calc.u32MainCSize;
    v->u64PhyAddr[0]=CVI_VB_Handle2PhysAddr(block);
    v->u64PhyAddr[1]=v->u64PhyAddr[0]+ALIGN(calc.u32MainYSize,calc.u16AddrAlign);
    input=CVI_SYS_MmapCache(v->u64PhyAddr[0],calc.u32VBSize);if(!input||input==MAP_FAILED){input=NULL;goto done;}
    /* Four distinct quadrants check rotation, crop and RGB channel order. */
    const uint8_t yuv[4][3]={{82,240,90},{145,34,54},{41,110,240},{235,128,128}};
    for(unsigned y=0;y<1080;y++)for(unsigned x=0;x<1920;x++)input[y*v->u32Stride[0]+x]=yuv[(y>=540)*2+(x>=960)][0];
    uint8_t *uv=input+(v->u64PhyAddr[1]-v->u64PhyAddr[0]);
    for(unsigned y=0;y<540;y++)for(unsigned x=0;x<1920;x+=2){unsigned q=(y>=270)*2+(x>=960);uv[y*v->u32Stride[1]+x]=yuv[q][1];uv[y*v->u32Stride[1]+x+1]=yuv[q][2];}
    CHECK(CVI_SYS_IonFlushCache(v->u64PhyAddr[0],input,calc.u32VBSize));
    if(argc==2&&!strcmp(argv[1],"--rotation")) {
        scaled=1;CHECK(media_scale_start(CAMERA_IR));
        rc=rotation_check(&frame);goto done;
    }
    uint8_t *reference=malloc(UI_WIDTH*UI_HEIGHT*3),*nv12=malloc(540*960*3/2),*ref_nv12=malloc(540*960*3/2),*gray=malloc(540*960),*ref_gray=malloc(540*960);
    if(!reference||!nv12||!ref_nv12||!gray||!ref_gray)goto done;
    for(int hardware=0;hardware<=1;hardware++) {
        if(hardware){unsetenv("KY50_PREVIEW_CSC");unsetenv("KY50_ROTATION");}
        else {setenv("KY50_PREVIEW_CSC","software",1);setenv("KY50_ROTATION","software",1);}
        scaled=1;CHECK(media_scale_start(CAMERA_RGB));
        VPSS_CHN_ATTR_S attr;CHECK(CVI_VPSS_GetChnAttr(0,0,&attr));
        if(attr.enPixelFormat!=PIXEL_FORMAT_NV21)goto done;
        double wall=ms(CLOCK_MONOTONIC),cpu=ms(CLOCK_PROCESS_CPUTIME_ID);
        int iterations=hardware?1000:60;
        for(int i=0;i<iterations;i++) {
            CHECK(media_scale_frame(&frame,rgb,NULL,0));
            if(hardware)for(int y=0;y<UI_HEIGHT;y+=8)for(int x=0;x<UI_WIDTH;x+=8) {
                if(abs(x-241)<4||abs(y-428)<4)continue;
                for(int c=0;c<3;c++)if(abs((int)rgb[(y*UI_WIDTH+x)*3+c]-reference[(y*UI_WIDTH+x)*3+c])>3) {
                    fprintf(stderr,"CSC_FRAME_BAD frame=%d x=%d y=%d c=%d old=%d new=%d\n",i,x,y,c,reference[(y*UI_WIDTH+x)*3+c],rgb[(y*UI_WIDTH+x)*3+c]);goto done;
                }
            }
        }
        printf("CSC_BENCH hardware=%d wall_ms=%.3f cpu_ms=%.3f\n",hardware,(ms(CLOCK_MONOTONIC)-wall)/iterations,(ms(CLOCK_PROCESS_CPUTIME_ID)-cpu)/iterations);
        if(hardware) {
            CHECK(CVI_VPSS_GetChnAttr(2,0,&attr));if(attr.enPixelFormat!=PIXEL_FORMAT_RGB_888)goto done;
            int errors=0;
            for(int y=0;y<UI_HEIGHT;y++)for(int x=0;x<UI_WIDTH;x++) {
                if(abs(x-241)<4||abs(y-428)<4)continue;
                for(int c=0;c<3;c++)if(abs((int)rgb[(y*UI_WIDTH+x)*3+c]-reference[(y*UI_WIDTH+x)*3+c])>3)++errors;
            }
            printf("CSC_SPATIAL errors=%d\n",errors);if(errors)goto done;
        } else memcpy(reference,rgb,UI_WIDTH*UI_HEIGHT*3);
        for(int i=0;i<30;i++) {
            CHECK(media_ir_half_frame(&frame,gray));CHECK(media_rgb_half_frame(&frame,nv12));
            if(!hardware){memcpy(ref_gray,gray,540*960);memcpy(ref_nv12,nv12,540*960*3/2);}
            else if(memcmp(ref_gray,gray,540*960)||memcmp(ref_nv12,nv12,540*960*3/2)) {
                fprintf(stderr,"ALGORITHM hardware rotation differs from software frame=%d\n",i);
                int gy=0,gc=0,first_y=-1,first_c=-1;
                for(int k=0;k<540*960;k++)if(ref_gray[k]!=gray[k]){++gy;if(first_y<0)first_y=k;}
                for(int k=0;k<540*960*3/2;k++)if(ref_nv12[k]!=nv12[k]){++gc;if(first_c<0)first_c=k;}
                fprintf(stderr,"DIFF gray_count=%d first=%d old=%d new=%d color_count=%d first=%d old=%d new=%d\n",gy,first_y,first_y<0?-1:ref_gray[first_y],first_y<0?-1:gray[first_y],gc,first_c,first_c<0?-1:ref_nv12[first_c],first_c<0?-1:nv12[first_c]);goto done;
            }
        }
        printf("PASS algorithm IR Y and RGB NV12 hardware=%d 30 paired frames byte-exact\n",hardware);
        /* Switching RGB/IR preview shares scalers without restarting them. */
        for(int i=0;i<30;i++)CHECK(media_scale_frame(&frame,rgb,NULL,1));
        for(int i=0;i<UI_WIDTH*UI_HEIGHT;i++)if(rgb[3*i]!=rgb[3*i+1]||rgb[3*i]!=rgb[3*i+2])goto done;
        CHECK(media_scale_frame(&frame,rgb,NULL,0));
        int max_error=0;
        for(int y=100;y<=700;y+=600)for(int x=100;x<=380;x+=280) {
            unsigned quadrant=(x>=240)*2+(y<427);uint8_t ys[8],vus[8],expected[24];
            memset(ys,yuv[quadrant][0],8);for(int i=0;i<8;i+=2){vus[i]=yuv[quadrant][1];vus[i+1]=yuv[quadrant][2];}
            pixels_nv21(expected,ys,vus,8);
            printf("CSC_COLOR hardware=%d q=%u expected=%u,%u,%u actual=%u,%u,%u\n",hardware,quadrant,
                expected[0],expected[1],expected[2],rgb[(y*UI_WIDTH+x)*3],rgb[(y*UI_WIDTH+x)*3+1],rgb[(y*UI_WIDTH+x)*3+2]);
            for(int c=0;c<3;c++){int error=abs((int)rgb[(y*UI_WIDTH+x)*3+c]-expected[c]);if(error>max_error)max_error=error;}
        }
        printf("CSC_QUADRANTS hardware=%d max_error=%d\n",hardware,max_error);
        if(max_error>3)goto done;
        media_scale_stop();scaled=0;
    }
    free(reference);free(nv12);free(ref_nv12);free(gray);free(ref_gray);
    puts("PASS VPSS preview CSC, RGB channel order, CCW rotation/crop and padded strides");rc=0;
done:
    if(scaled)media_scale_stop();
    if(input)CVI_SYS_Munmap(input,calc.u32VBSize);
    if(block!=VB_INVALID_HANDLE)CVI_VB_ReleaseBlock(block);
    if(vb)CVI_VB_Exit();
    if(system)CVI_SYS_Exit();
    free(rgb);close(lock);return rc;
}
