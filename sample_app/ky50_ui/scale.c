#include "media.h"
#include "ui.h"
#include "pixels.h"
#include "hand_overlay.h"
#include "cvi_vpss.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_buffer.h"
#include "cvi_gdc.h"
#include "../../../build/media/include/internal/mpi_uapi/vpss_uapi.h"
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
static uint64_t scale_us(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
static uint64_t stage_time[2][5];
static unsigned stage_count[2];

struct scaler {
    int group,created,enabled,started,attached,reported;
    unsigned scaled_width,scaled_height;
    VB_POOL scale_pool,rotation_pool;
    int rotated_hw,gdc_attached;
    unsigned char *rotated;
};
static struct scaler preview={.group=0,.scale_pool=VB_INVALID_POOLID,.rotation_pool=VB_INVALID_POOLID};
static struct scaler algorithm_half={.group=1,.scale_pool=VB_INVALID_POOLID,.rotation_pool=VB_INVALID_POOLID};
static struct scaler color={.group=2,.scale_pool=VB_INVALID_POOLID,.rotation_pool=VB_INVALID_POOLID};
static struct scaler face_color={.group=3,.scale_pool=VB_INVALID_POOLID,.rotation_pool=VB_INVALID_POOLID};
static pthread_mutex_t face_color_mutex=PTHREAD_MUTEX_INITIALIZER;
static int hardware_rotation,hardware_color;
static void scaler_stop(struct scaler *s);
#define CHECK(x) do { int rc=(x);if(rc){fprintf(stderr,"%s: %#x\n",#x,rc);return -1;} }while(0)
static int preview_csc(int group)
{
    /* This BSP's default "601_LIMIT" matrix treats Y as full range. Match
     * pixels_nv21's limited-range Y input explicitly, on preview group only.
     * Public ProcAmp exposes integer brightness/contrast, not an exact matrix;
     * use the shipped BSP UAPI and fall back to NV21 if it is unsupported. */
    struct vpss_grp_csc_cfg csc={.VpssGrp=group,.enable=1,
        .proc_amp={50,50,50,50},.sub={16,128,128},
        .coef={{1192,0,1636},{1192,8192|400,8192|832},{1192,2064,0}}};
    int fd=open("/dev/soph-vpss",O_RDWR|O_CLOEXEC);if(fd<0)return -1;
    int rc=ioctl(fd,VPSS_SET_GRP_CSC_CFG,&csc);close(fd);return rc;
}
static int scaler_start(struct scaler *s,unsigned input_w,unsigned input_h,
                        unsigned w,unsigned h,PIXEL_FORMAT_E format)
{
    s->scaled_width=w;s->scaled_height=h;
    /* This BSP checks group dimensions against each input frame exactly. */
    VPSS_GRP_ATTR_S group={.u32MaxW=input_w,.u32MaxH=input_h,.enPixelFormat=format,.stFrameRate={-1,-1}};
    VPSS_CHN_ATTR_S ch={.u32Width=w,.u32Height=h,.enVideoFormat=VIDEO_FORMAT_LINEAR,
        .enPixelFormat=s==&face_color?PIXEL_FORMAT_BGR_888:s==&color?PIXEL_FORMAT_RGB_888:PIXEL_FORMAT_NV21,.stFrameRate={-1,-1},.u32Depth=2,
        .stAspectRatio={.enMode=ASPECT_RATIO_NONE}};
    CHECK(CVI_VPSS_CreateGrp(s->group,&group));s->created=1;
    CHECK(CVI_VPSS_SetChnAttr(s->group,0,&ch));
    VB_POOL_CONFIG_S pool={.u32BlkCnt=s==&face_color?2:4,.enRemapMode=VB_REMAP_MODE_CACHED};
    /* With GDC enabled the BSP pads the VPSS intermediate frame in BOTH
     * dimensions too. Allocating just stride * original height corrupts UV
     * across VB blocks even if the separate GDC output pool is large enough. */
    const int csc_only=s==&color||s==&face_color;
    unsigned pool_w=hardware_rotation&&!csc_only?ALIGN(w,DEFAULT_ALIGN):w;
    unsigned pool_h=hardware_rotation&&!csc_only?ALIGN(h,DEFAULT_ALIGN):h;
    pool.u32BlkSize=COMMON_GetPicBufferSize(pool_w,pool_h,ch.enPixelFormat,
        DATA_BITWIDTH_8,COMPRESS_MODE_NONE,DEFAULT_ALIGN);
    s->scale_pool=CVI_VB_CreatePool(&pool);
    if(s->scale_pool==VB_INVALID_POOLID) return -1;
    CHECK(CVI_VPSS_AttachVbPool(s->group,0,s->scale_pool));s->attached=1;
    if(csc_only) {
        CHECK(preview_csc(s->group));
    } else if(hardware_rotation) {
        /* GDC uses its own output pool, distinct from the VPSS output pool.
         * Both swapped dimensions must be aligned, including padded height. */
        pool.u32BlkCnt=3;
        pool.u32BlkSize=COMMON_GetPicBufferSize(ALIGN(h,DEFAULT_ALIGN),ALIGN(w,DEFAULT_ALIGN),
            PIXEL_FORMAT_NV21,DATA_BITWIDTH_8,COMPRESS_MODE_NONE,DEFAULT_ALIGN);
        s->rotation_pool=CVI_VB_CreatePool(&pool);
        if(s->rotation_pool==VB_INVALID_POOLID)return -1;
        MMF_CHN_S channel={.enModId=CVI_ID_VPSS,.s32DevId=s->group,.s32ChnId=0};
        CHECK(CVI_GDC_AttachVbPool(&channel,s->rotation_pool));s->gdc_attached=1;
        CHECK(CVI_VPSS_SetChnRotation(s->group,0,ROTATION_270));s->rotated_hw=1;
    } else CHECK(CVI_VPSS_SetChnRotation(s->group,0,ROTATION_0));
    if(s==&preview&&!s->rotated_hw) {
        s->rotated=malloc((size_t)w*h*3/2);
        if(!s->rotated)return -1;
    }
    CHECK(CVI_VPSS_EnableChn(s->group,0));s->enabled=1;
    CHECK(CVI_VPSS_StartGrp(s->group));s->started=1;
    return 0;
}
static int start_scalers(enum camera_source source)
{
    if(scaler_start(&preview,source==CAMERA_DVP?640:1920,source==CAMERA_DVP?480:1080,
        864,source==CAMERA_DVP?648:486,source==CAMERA_DVP?PIXEL_FORMAT_YUYV:PIXEL_FORMAT_NV21))return -1;
    if(source!=CAMERA_DVP&&scaler_start(&algorithm_half,1920,1080,960,540,PIXEL_FORMAT_NV21))return -1;
    return 0;
}
int media_scale_start(enum camera_source source)
{
    const char *rotation=getenv("KY50_ROTATION"),*csc=getenv("KY50_PREVIEW_CSC");
    /* DVP retains its separately validated software path. RGB and IR share
     * group0, so it must remain NV21 even when preview switches without restart. */
    hardware_rotation=source!=CAMERA_DVP&&(!rotation||strcmp(rotation,"software"));
    hardware_color=hardware_rotation&&(!csc||strcmp(csc,"software"));
    if(start_scalers(source)) {
        media_scale_stop();
        if(!hardware_rotation)return -1;
        hardware_rotation=hardware_color=0;
        if(start_scalers(source))return -1;
        fprintf(stderr,"GDC initialization failed: using software rotation/CSC\n");
    }
    printf("ROTATION_PIPELINE algorithm_and_preview_rotation=%s preview_csc=%s\n",hardware_rotation?"GDC CCW90":"software CCW90",hardware_color?"VPSS RGB888":"software");
    return 0;
}
static int display_color(const VIDEO_FRAME_INFO_S *input,uint8_t *rgb,unsigned x,unsigned y)
{
    const VIDEO_FRAME_S *in=&input->stVFrame;
    if(!color.started) {
        if(scaler_start(&color,in->u32Width,in->u32Height,UI_WIDTH,UI_HEIGHT,PIXEL_FORMAT_NV21)) {
            scaler_stop(&color);return -1;
        }
        /* VPSS adds the frame storage offsets itself; crop is content-relative. */
        VPSS_CROP_INFO_S crop={.bEnable=1,.enCropCoordinate=VPSS_CROP_ABS_COOR,
            .stCropRect={.s32X=x,.s32Y=y,.u32Width=UI_WIDTH,.u32Height=UI_HEIGHT}};
        CHECK(CVI_VPSS_SetGrpCrop(color.group,&crop));
        printf("PREVIEW_CSC hardware VPSS RGB888 input=%ux%u crop=%u,%u %ux%u\n",in->u32Width,in->u32Height,x,y,UI_WIDTH,UI_HEIGHT);
    }
    VIDEO_FRAME_INFO_S frame={0};
    CHECK(CVI_VPSS_SendFrame(color.group,input,100));
    CHECK(CVI_VPSS_GetChnFrame(color.group,0,&frame,100));
    VIDEO_FRAME_S *v=&frame.stVFrame;int rc=-1;
    uint8_t *p=NULL;unsigned length=v->u32Stride[0]*v->u32Height;
    if(v->enPixelFormat!=PIXEL_FORMAT_RGB_888||v->u32Width!=UI_WIDTH||v->u32Height!=UI_HEIGHT||
        v->u32Stride[0]<UI_WIDTH*3||v->u32Length[0]<length)goto done;
    p=CVI_SYS_MmapCache(v->u64PhyAddr[0],length);
    if(!p||p==MAP_FAILED){p=NULL;goto done;}
    if(CVI_SYS_IonInvalidateCache(v->u64PhyAddr[0],p,length))goto done;
    for(unsigned row=0;row<UI_HEIGHT;row++)memcpy(rgb+(size_t)row*UI_WIDTH*3,p+(size_t)row*v->u32Stride[0],UI_WIDTH*3);
    rc=0;
done:
    if(p)CVI_SYS_Munmap(p,length);
    if(CVI_VPSS_ReleaseChnFrame(color.group,0,&frame))rc=-1;
    return rc;
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
       (int)v->u32Width-left-right!=(int)(s->rotated_hw?s->scaled_height:s->scaled_width)||(int)v->u32Height-top-bottom!=(int)(s->rotated_hw?s->scaled_width:s->scaled_height)||
       v->u32Stride[0]<v->u32Width||((rgb||!monochrome)&&(v->u32Stride[1]<v->u32Width||(left|top)&1))) {
        fprintf(stderr,"Unexpected scaled group%d frame %ux%u fmt=%d\n",s->group,v->u32Width,v->u32Height,v->enPixelFormat);goto done;
    }
    unsigned crop_x=rgb?((s->scaled_height-UI_WIDTH)/2)&~1U:0;
    unsigned crop_y=rgb?4:0;
    if(crop_x+w>s->scaled_height||crop_y+h>s->scaled_width) goto done;
    if(!s->reported++) printf("ROTATED group=%d source=%ux%u stride=%u/%u offsets=L%d R%d T%d B%d rotated=%ux%u copy=%u,%u %ux%u%s\n",
        s->group,v->u32Width,v->u32Height,v->u32Stride[0],v->u32Stride[1],left,right,top,bottom,
        s->scaled_height,s->scaled_width,crop_x,crop_y,w,h,rgb?" display crop":monochrome?" full IR half-size":" full RGB NV12 half-size");
    if(rgb&&!monochrome&&s->rotated_hw&&hardware_color) {
        ts[2]=ts[3]=scale_us();
        if(!display_color(&out,rgb,crop_x,crop_y)) {ts[4]=scale_us();goto mapped;}
        /* Keep recognition and preview available if the optional CSC group fails. */
        scaler_stop(&color);hardware_color=0;
        fprintf(stderr,"Preview CSC failed: using software color conversion\n");
    }
    int audit_mono=rgb&&monochrome&&s->reported==30;
    for(int i=0;i<(!monochrome||audit_mono?2:1);i++) {
        if(v->u32Length[i]<len[i]) goto done;
        p[i]=CVI_SYS_MmapCache(v->u64PhyAddr[i],len[i]);
        if(!p[i]||p[i]==MAP_FAILED) { p[i]=NULL;goto done; }
        if(CVI_SYS_IonInvalidateCache(v->u64PhyAddr[i],p[i],len[i])) goto done;
    }
    if(audit_mono) {
        unsigned low=255,high=0;
        unsigned content_w=v->u32Width-left-right,content_h=v->u32Height-top-bottom;
        for(unsigned y=0;y<content_h/2;y++)for(unsigned x=0;x<content_w;x++) {
            unsigned c=p[1][(y+top/2)*v->u32Stride[1]+left+x];
            if(c<low)low=c;
            if(c>high)high=c;
        }
        printf("IR_MONO ISP output chroma_min=%u chroma_max=%u neutral=128; preview=Y-only grayscale\n",low,high);
    }
    ts[2]=scale_us();
    if(s->rotated_hw) {
        const uint8_t *y=p[0]+(size_t)top*v->u32Stride[0]+left;
        const uint8_t *vu=monochrome?NULL:p[1]+(size_t)(top/2)*v->u32Stride[1]+left;
        if(gray) {
            if(monochrome) {
                if(hand_copy_y(gray,w,h,y,w,h,v->u32Stride[0],0,0))goto done;
            } else if(color_nv21_copy(gray,y,vu,w,h,v->u32Stride[0],v->u32Stride[1]))goto done;
            ts[3]=ts[4]=scale_us();
        } else {
            ts[3]=scale_us();
            for(unsigned row=0;row<h;row++) {
                const uint8_t *l=y+(size_t)(row+crop_y)*v->u32Stride[0]+crop_x;
                if(monochrome)pixels_gray(rgb+(size_t)row*w*3,l,w);
                else pixels_nv21(rgb+(size_t)row*w*3,l,vu+(size_t)((row+crop_y)/2)*v->u32Stride[1]+crop_x,w);
            }
            ts[4]=scale_us();
        }
        goto mapped;
    }
    unsigned char *rotated=gray?gray:s->rotated;
    if(gray&&!monochrome) {
        if(color_nv21_ccw90(gray,p[0]+(size_t)top*v->u32Stride[0]+left,
            p[1]+(size_t)(top/2)*v->u32Stride[1]+left,s->scaled_width,s->scaled_height,
            v->u32Stride[0],v->u32Stride[1]))goto done;
    } else if(hand_rotate_ccw90(rotated,p[0]+(size_t)top*v->u32Stride[0]+left,
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
mapped:
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
    return scaler_frame(&algorithm_half,input,NULL,gray,NULL,1);
}
int media_rgb_half_frame(const VIDEO_FRAME_INFO_S *input,unsigned char *nv12)
{
    if(input->stVFrame.u32Width!=1920||input->stVFrame.u32Height!=1080)return -1;
    return scaler_frame(&algorithm_half,input,NULL,nv12,NULL,0);
}
static void scaler_stop(struct scaler *s)
{
    if(s->started) CVI_VPSS_StopGrp(s->group);
    if(s->enabled) CVI_VPSS_DisableChn(s->group,0);
    if(s->attached) CVI_VPSS_DetachVbPool(s->group,0);
    if(s->gdc_attached) {
        MMF_CHN_S channel={.enModId=CVI_ID_VPSS,.s32DevId=s->group,.s32ChnId=0};
        CVI_GDC_DetachVbPool(&channel);
    }
    if(s->created) CVI_VPSS_DestroyGrp(s->group);
    if(s->rotation_pool!=VB_INVALID_POOLID)CVI_VB_DestroyPool(s->rotation_pool);
    if(s->scale_pool!=VB_INVALID_POOLID) CVI_VB_DestroyPool(s->scale_pool);
    free(s->rotated);s->rotated=NULL;
    s->scale_pool=s->rotation_pool=VB_INVALID_POOLID;
    s->gdc_attached=s->rotated_hw=0;
    s->attached=s->started=s->enabled=s->created=s->reported=0;
}
void media_scale_stop(void) { scaler_stop(&color);scaler_stop(&algorithm_half);scaler_stop(&preview);
    media_face_color_stop();
    memset(stage_time,0,sizeof(stage_time));memset(stage_count,0,sizeof(stage_count)); }

/* Independent group for immutable worker snapshots. Never retains a camera frame. */
static VB_POOL face_input_pool=VB_INVALID_POOLID;
static VB_BLK face_input_block=VB_INVALID_HANDLE;
static VIDEO_FRAME_INFO_S face_input;
static uint8_t* face_input_map;
static unsigned face_input_bytes;
static void face_color_close(void)
{
    scaler_stop(&face_color);
    if(face_input_map)CVI_SYS_Munmap(face_input_map,face_input_bytes);
    face_input_map=NULL;
    if(face_input_block!=VB_INVALID_HANDLE)CVI_VB_ReleaseBlock(face_input_block);
    face_input_block=VB_INVALID_HANDLE;
    if(face_input_pool!=VB_INVALID_POOLID)CVI_VB_DestroyPool(face_input_pool);
    face_input_pool=VB_INVALID_POOLID;memset(&face_input,0,sizeof(face_input));face_input_bytes=0;
}
void media_face_color_stop(void)
{
    pthread_mutex_lock(&face_color_mutex);face_color_close();pthread_mutex_unlock(&face_color_mutex);
}
int media_face_color_nv12(const uint8_t* nv12,uint16_t w,uint16_t h,uint8_t* bgr,uint32_t stride)
{
    if(!nv12||!bgr||!w||!h||((w|h)&1)||stride<(unsigned)w*3)return -1;
    pthread_mutex_lock(&face_color_mutex);
    uint64_t ticks[4];ticks[0]=scale_us();int rc=-1,have_output=0,cold=!face_color.started;
    VIDEO_FRAME_INFO_S output={0};uint8_t* mapped=NULL;unsigned length=0;
    if(face_color.started&&(face_color.scaled_width!=w||face_color.scaled_height!=h))face_color_close();
    if(!face_color.started) {
        VB_CAL_CONFIG_S calc;COMMON_GetPicBufferConfig(w,h,PIXEL_FORMAT_NV12,DATA_BITWIDTH_8,
            COMPRESS_MODE_NONE,DEFAULT_ALIGN,&calc);
        VB_POOL_CONFIG_S pool={.u32BlkSize=calc.u32VBSize,.u32BlkCnt=1,.enRemapMode=VB_REMAP_MODE_CACHED};
        face_input_pool=CVI_VB_CreatePool(&pool);if(face_input_pool==VB_INVALID_POOLID)goto failed;
        face_input_block=CVI_VB_GetBlock(face_input_pool,calc.u32VBSize);if(face_input_block==VB_INVALID_HANDLE)goto failed;
        face_input.u32PoolId=CVI_VB_Handle2PoolId(face_input_block);
        VIDEO_FRAME_S* f=&face_input.stVFrame;f->u32Width=w;f->u32Height=h;f->enPixelFormat=PIXEL_FORMAT_NV12;
        f->u32Stride[0]=calc.u32MainStride;f->u32Stride[1]=calc.u32CStride;
        f->u32Length[0]=calc.u32MainYSize;f->u32Length[1]=calc.u32MainCSize;
        f->u64PhyAddr[0]=CVI_VB_Handle2PhysAddr(face_input_block);
        f->u64PhyAddr[1]=f->u64PhyAddr[0]+ALIGN(calc.u32MainYSize,calc.u16AddrAlign);
        face_input_bytes=calc.u32VBSize;
        face_input_map=CVI_SYS_MmapCache(f->u64PhyAddr[0],face_input_bytes);
        if(!face_input_map||face_input_map==MAP_FAILED){face_input_map=NULL;goto failed;}
        memset(face_input_map,128,face_input_bytes);
        if(scaler_start(&face_color,w,h,w,h,PIXEL_FORMAT_NV12))goto failed;
    }
    VIDEO_FRAME_S* f=&face_input.stVFrame;
    uint8_t* uv=face_input_map+(f->u64PhyAddr[1]-f->u64PhyAddr[0]);
    for(unsigned y=0;y<h;++y) {
        const uint8_t* src=nv12+(size_t)y*w;uint8_t* dst=face_input_map+(size_t)y*f->u32Stride[0];unsigned x=0;
        // Match the SDK's max(0,Y-16), including below-black camera codes.
#if defined(__ARM_NEON)
        for(;x+16<=w;x+=16)vst1q_u8(dst+x,vmaxq_u8(vld1q_u8(src+x),vdupq_n_u8(16)));
#endif
        for(;x<w;++x)dst[x]=src[x]<16?16:src[x];
    }
    for(unsigned y=0;y<h/2;++y)memcpy(uv+(size_t)y*f->u32Stride[1],nv12+(size_t)w*h+(size_t)y*w,w);
    if(CVI_SYS_IonFlushCache(f->u64PhyAddr[0],face_input_map,face_input_bytes))goto failed;
    ticks[1]=scale_us();
    if(CVI_VPSS_SendFrame(face_color.group,&face_input,100))goto failed;
    if(CVI_VPSS_GetChnFrame(face_color.group,0,&output,100))goto failed;
    have_output=1;ticks[2]=scale_us();
    VIDEO_FRAME_S* out=&output.stVFrame;length=out->u32Stride[0]*out->u32Height;
    if(out->enPixelFormat!=PIXEL_FORMAT_BGR_888||out->u32Width!=w||out->u32Height!=h||
       out->u32Stride[0]<(unsigned)w*3||out->u32Length[0]<length)goto failed;
    mapped=CVI_SYS_MmapCache(out->u64PhyAddr[0],length);
    if(!mapped||mapped==MAP_FAILED){mapped=NULL;goto failed;}
    if(CVI_SYS_IonInvalidateCache(out->u64PhyAddr[0],mapped,length))goto failed;
    for(unsigned y=0;y<h;++y)memcpy(bgr+(size_t)y*stride,mapped+(size_t)y*out->u32Stride[0],(size_t)w*3);
    CVI_SYS_Munmap(mapped,length);mapped=NULL;
    rc=CVI_VPSS_ReleaseChnFrame(face_color.group,0,&output);have_output=0;ticks[3]=scale_us();
    if(getenv("KY50_TRACE_STALL"))printf("FACE_CSC cold=%d prepare_us=%llu hardware_us=%llu copy_release_us=%llu total_us=%llu result=%d\n",
        cold,(unsigned long long)(ticks[1]-ticks[0]),(unsigned long long)(ticks[2]-ticks[1]),
        (unsigned long long)(ticks[3]-ticks[2]),(unsigned long long)(ticks[3]-ticks[0]),rc);
    if(!rc){pthread_mutex_unlock(&face_color_mutex);return 0;}
failed:
    if(mapped)CVI_SYS_Munmap(mapped,length);
    if(have_output)CVI_VPSS_ReleaseChnFrame(face_color.group,0,&output);
    fprintf(stderr,"FACE_CSC conversion failed; feature job rejected\n");
    face_color_close();pthread_mutex_unlock(&face_color_mutex);return -1;
}
