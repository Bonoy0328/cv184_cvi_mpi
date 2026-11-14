#include "gc0328.h"
#include "cvi_vi.h"
#include <stdio.h>
static int dev_up,pipe_up,started,channel_up;
#define CHECK(x) do { int r=(x);if(r){fprintf(stderr,"%s: %#x\n",#x,r);return -1;} }while(0)
int gc0328_start(void) {
    VI_DEV_ATTR_S dev={.snrFps=15,.stSize={640,480},.enIntfMode=VI_MODE_MIPI,
        .enInputDataType=VI_DATA_TYPE_RGB,.enDataSeq=VI_DATA_SEQ_YUYV,
        .enWorkMode=VI_WORK_MODE_1Multiplex,.enYuvSceneMode=VI_ISP_YUV_SCENE_BYPASS};
    VI_DEV_BIND_PIPE_S bind={.u32Num=1,.MipiDev=0};
    for(int i=0;i<VI_MAX_PIPE_NUM;i++) bind.PipeId[i]=i?-1:0;
    CHECK(CVI_VI_EnablePatgen(0));CHECK(CVI_VI_SetDevAttr(0,&dev));CHECK(CVI_VI_SetDevBindAttr(0,&bind));
    CHECK(CVI_VI_EnableDev(0));dev_up=1;
    VI_PIPE_ATTR_S pipe={.u32MaxW=640,.u32MaxH=480,.enPixFmt=PIXEL_FORMAT_RGB_BAYER_12BPP,
        .enBitWidth=DATA_BITWIDTH_12,.stFrameRate={-1,-1},.bNrEn=CVI_TRUE,
        .bYuvBypassPath=CVI_FALSE,.enCompressMode=COMPRESS_MODE_TILE};
    CHECK(CVI_VI_CreatePipe(0,&pipe));pipe_up=1;
    CHECK(CVI_VI_StartPipe(0));started=1;
    VI_CHN_ATTR_S channel={.stSize={640,480},.enPixelFormat=PIXEL_FORMAT_NV21,
        .enDynamicRange=DYNAMIC_RANGE_SDR8,.enVideoFormat=VIDEO_FORMAT_LINEAR,
        .enCompressMode=COMPRESS_MODE_TILE,.u32Depth=2,.u32BindVbPool=(unsigned)-1};
    CHECK(CVI_VI_SetChnAttr(0,0,&channel));CHECK(CVI_VI_EnableChn(0,0));channel_up=1;
return 0; }
void gc0328_stop(void) {if(channel_up)CVI_VI_DisableChn(0,0);if(started)CVI_VI_StopPipe(0);if(pipe_up)CVI_VI_DestroyPipe(0);if(dev_up)CVI_VI_DisableDev(0);}
