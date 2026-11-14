#include "media.h"
#include "ui.h"
#include "pixels.h"
#include "hand_overlay.h"
#include "cvi_vpss.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_buffer.h"
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
static uint64_t scale_us(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
static uint64_t stage_time[2][5];
static unsigned stage_count[2];

struct scaler {
    int group,created,enabled,started,attached,reported;
    unsigned scaled_width,scaled_height;
    VB_POOL scale_pool;
    unsigned char *rotated;
};
static struct scaler preview={.group=0,.scale_pool=VB_INVALID_POOLID};
static struct scaler ir_half={.group=1,.scale_pool=VB_INVALID_POOLID};
#define CHECK(x) do { int rc=(x);if(rc){fprintf(stderr,"%s: %#x\n",#x,rc);return -1;} }while(0)
static int scaler_start(struct scaler *s,unsigned input_w,unsigned input_h,
                        unsigned w,unsigned h,PIXEL_FORMAT_E format)
{
    s->scaled_width=w;s->scaled_height=h;
    /* This BSP checks group dimensions against each input frame exactly. */
    VPSS_GRP_ATTR_S group={.u32MaxW=input_w,.u32MaxH=input_h,.enPixelFormat=format,.stFrameRate={-1,-1}};
    VPSS_CHN_ATTR_S ch={.u32Width=w,.u32Height=h,.enVideoFormat=VIDEO_FORMAT_LINEAR,
        .enPixelFormat=PIXEL_FORMAT_NV21,.stFrameRate={-1,-1},.u32Depth=2,
        .stAspectRatio={.enMode=ASPECT_RATIO_NONE}};
    CHECK(CVI_VPSS_CreateGrp(s->group,&group));s->created=1;
    CHECK(CVI_VPSS_SetChnAttr(s->group,0,&ch));
    VB_POOL_CONFIG_S pool={.u32BlkCnt=4,.enRemapMode=VB_REMAP_MODE_CACHED};
    pool.u32BlkSize=COMMON_GetPicBufferSize(w,h,PIXEL_FORMAT_NV21,
        DATA_BITWIDTH_8,COMPRESS_MODE_NONE,DEFAULT_ALIGN);
    s->scale_pool=CVI_VB_CreatePool(&pool);
    if(s->scale_pool==VB_INVALID_POOLID) return -1;
    CHECK(CVI_VPSS_AttachVbPool(s->group,0,s->scale_pool));s->attached=1;
    /* GDC rotation intermittently returns an unrotated descriptor then blocks
     * subsequent jobs on this BSP. Rotate VPSS output in application memory. */
    CHECK(CVI_VPSS_SetChnRotation(s->group,0,ROTATION_0));
    if(s==&preview) {
        s->rotated=malloc((size_t)w*h*3/2);
        if(!s->rotated) return -1;
    }
    CHECK(CVI_VPSS_EnableChn(s->group,0));s->enabled=1;
    CHECK(CVI_VPSS_StartGrp(s->group));s->started=1;
    return 0;
}
int media_scale_start(enum camera_source source)
{
    /* Display retains its existing aspect fill / center crop. */
    if(scaler_start(&preview,source==CAMERA_DVP?640:1920,source==CAMERA_DVP?480:1080,
                    864,source==CAMERA_DVP?648:486,
                    source==CAMERA_DVP?PIXEL_FORMAT_YUYV:PIXEL_FORMAT_NV21)) return -1;
    /* Full IR frame at half size: 1920x1080 -> 960x540 -> CCW90 540x960.
     * No content is cropped. This group never receives the RGB VI frame. */
    if(source!=CAMERA_DVP && scaler_start(&ir_half,1920,1080,960,540,PIXEL_FORMAT_NV21)) return -1;
    return 0;
}
static int scaler_frame(struct scaler *s,const VIDEO_FRAME_INFO_S *input,
                         unsigned char *rgb,unsigned char *gray,HandViewport *view,int monochrome)
{
    if(!s->started) return -1;
    uint64_t ts[6];ts[0]=scale_us();
    VIDEO_FRAME_INFO_S out={0};
    CHECK(CVI_VPSS_SendFrame(s->group,input,100));
    CHECK(CVI_VPSS_GetChnFrame(s->group,0,&out,100));
    ts[1]=scale_us();
    VIDEO_FRAME_S *v=&out.stVFrame;
    unsigned char *p[2]={NULL,NULL};
    unsigned len[2]={v->u32Stride[0]*v->u32Height,v->u32Stride[1]*(v->u32Height/2)};
    int rc=-1;
    int left=v->s16OffsetLeft,right=v->s16OffsetRight;
    int top=v->s16OffsetTop,bottom=v->s16OffsetBottom;
    unsigned w=rgb?UI_WIDTH:HAND_INPUT_WIDTH,h=rgb?UI_HEIGHT:HAND_INPUT_HEIGHT;
    if(v->enPixelFormat!=PIXEL_FORMAT_NV21||left<0||right<0||top<0||bottom<0||
       (int)v->u32Width-left-right!=(int)s->scaled_width||(int)v->u32Height-top-bottom!=(int)s->scaled_height||
       v->u32Stride[0]<v->u32Width||(rgb&&(v->u32Stride[1]<v->u32Width||(left|top)&1))) {
        fprintf(stderr,"Unexpected scaled group%d frame %ux%u fmt=%d\n",s->group,v->u32Width,v->u32Height,v->enPixelFormat);goto done;
    }
    unsigned crop_x=rgb?((s->scaled_height-UI_WIDTH)/2)&~1U:0;
    unsigned crop_y=rgb?4:0;
    if(crop_x+w>s->scaled_height||crop_y+h>s->scaled_width) goto done;
    if(!s->reported++) printf("ROTATED group=%d software=CCW90 source=%ux%u stride=%u/%u offsets=L%d R%d T%d B%d rotated=%ux%u copy=%u,%u %ux%u%s\n",
        s->group,v->u32Width,v->u32Height,v->u32Stride[0],v->u32Stride[1],left,right,top,bottom,
        s->scaled_height,s->scaled_width,crop_x,crop_y,w,h,rgb?" display crop":" full IR half-size");
    int audit_mono=rgb&&monochrome&&s->reported==30;
    for(int i=0;i<(rgb&&(!monochrome||audit_mono)?2:1);i++) {
        if(v->u32Length[i]<len[i]) goto done;
        p[i]=CVI_SYS_MmapCache(v->u64PhyAddr[i],len[i]);
        if(!p[i]||p[i]==MAP_FAILED) { p[i]=NULL;goto done; }
        if(CVI_SYS_IonInvalidateCache(v->u64PhyAddr[i],p[i],len[i])) goto done;
    }
    if(audit_mono) {
        unsigned low=255,high=0;
        for(unsigned y=0;y<s->scaled_height/2;y++)for(unsigned x=0;x<s->scaled_width;x++) {
            unsigned c=p[1][(y+top/2)*v->u32Stride[1]+left+x];
            if(c<low)low=c;
            if(c>high)high=c;
        }
        printf("IR_MONO ISP output chroma_min=%u chroma_max=%u neutral=128; preview=Y-only grayscale\n",low,high);
    }
    ts[2]=scale_us();
    unsigned char *rotated=gray?gray:s->rotated;
    if(hand_rotate_ccw90(rotated,p[0]+(size_t)top*v->u32Stride[0]+left,
        s->scaled_width,s->scaled_height,v->u32Stride[0],1)) goto done;
    ts[3]=scale_us();
    if(rgb) {
        unsigned char *uv=rotated+(size_t)s->scaled_width*s->scaled_height;
        if(!monochrome&&hand_rotate_ccw90(uv,p[1]+(size_t)(top/2)*v->u32Stride[1]+left,
            s->scaled_width/2,s->scaled_height/2,v->u32Stride[1],2)) goto done;
        for(unsigned y=0;y<h;y++) {
            const unsigned char *l=rotated+(size_t)(y+crop_y)*s->scaled_height+crop_x;
            const unsigned char *vu=uv+(size_t)((y+crop_y)/2)*s->scaled_height+crop_x;
            if(monochrome) pixels_gray(rgb+y*w*3,l,w);
            else pixels_nv21(rgb+y*w*3,l,vu,w);
        }
    }
    ts[4]=scale_us();
    if(view) {
        view->input_width=HAND_INPUT_WIDTH;view->input_height=HAND_INPUT_HEIGHT;
        view->scaled_width=s->scaled_height;view->scaled_height=s->scaled_width;
        view->crop_x=crop_x;view->crop_y=crop_y;
    }
    rc=0;
done:
    for(int i=0;i<2;i++) if(p[i]) CVI_SYS_Munmap(p[i],len[i]);
    if(CVI_VPSS_ReleaseChnFrame(s->group,0,&out)) rc=-1;
    if(!rc) {
        ts[5]=scale_us();for(int i=0;i<5;i++)stage_time[s->group][i]+=ts[i+1]-ts[i];
        if(++stage_count[s->group]==100) {
            printf("SCALE_PERF group=%d vpss=%.2f map=%.2f rotate=%.2f convert=%.2f release=%.2f ms\n",s->group,
                stage_time[s->group][0]/100000.0,stage_time[s->group][1]/100000.0,stage_time[s->group][2]/100000.0,stage_time[s->group][3]/100000.0,stage_time[s->group][4]/100000.0);
            stage_count[s->group]=0;memset(stage_time[s->group],0,sizeof(stage_time[0]));
        }
    }
    return rc;
}
int media_scale_frame(const VIDEO_FRAME_INFO_S *input,unsigned char *rgb,HandViewport *view,int monochrome)
{
    return scaler_frame(&preview,input,rgb,NULL,view,monochrome);
}
int media_ir_half_frame(const VIDEO_FRAME_INFO_S *input,unsigned char *gray)
{
    if(input->stVFrame.u32Width!=1920||input->stVFrame.u32Height!=1080) return -1;
    return scaler_frame(&ir_half,input,NULL,gray,NULL,1);
}
static void scaler_stop(struct scaler *s)
{
    if(s->started) CVI_VPSS_StopGrp(s->group);
    if(s->enabled) CVI_VPSS_DisableChn(s->group,0);
    if(s->attached) CVI_VPSS_DetachVbPool(s->group,0);
    if(s->created) CVI_VPSS_DestroyGrp(s->group);
    if(s->scale_pool!=VB_INVALID_POOLID) CVI_VB_DestroyPool(s->scale_pool);
    free(s->rotated);s->rotated=NULL;
    s->scale_pool=VB_INVALID_POOLID;
    s->attached=s->started=s->enabled=s->created=s->reported=0;
}
void media_scale_stop(void) { scaler_stop(&ir_half);scaler_stop(&preview); }
