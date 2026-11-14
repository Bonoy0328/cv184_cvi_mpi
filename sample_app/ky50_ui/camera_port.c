/* KY50 GC2083 bring-up: SDK sensor callbacks and the sample_sensor VI/ISP sequence.
 * Explicit real sensors only; no internal pattern-generator fallback.
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/file.h>
#include <time.h>
#include <stdatomic.h>
#include "../ky50_sensor/sensor_board.h"
#include "media.h"
#include "gc0328.h"
#include "cvi_sensor.h"
#include "cvi_sns_ctrl.h"
#include "cvi_mipi.h"
#include "cvi_vi.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_buffer.h"
#include "cvi_isp.h"
#include "cvi_ae.h"
#include "cvi_awb.h"


extern ISP_SNS_OBJ_S stSnsGc2083_Obj;
static ISP_SNS_OBJ_S *sns = &stSnsGc2083_Obj;
static ISP_SENSOR_EXP_FUNC_S expfn;
/* Fixed identity in MIPI mode: pipe0 IR, pipe1 RGB, independent of preview selection. */
static int cameras=2, physical[2]={0,1}, preview_pipe, system_up, vb_up, thread_up;
static int dvp_selected;
static _Atomic uint64_t ir_capture_sequence;
static pthread_t isp_thread[2];
static struct {
	int registered, clock, dev, pipe, started, ae, awb, isp, channel;
	ALG_LIB_S ae_lib, awb_lib;
} state[2];

#define TRY(call) do { int result = (call); if (result) { \
	fprintf(stderr, "%s failed: %#x (line %d)\n", #call, result, __LINE__); return -1; \
} } while (0)

static void *run_isp(void *arg)
{
	int pipe = (int)(intptr_t)arg;
	/* Linux ISP shares an event loop, but Run must enable each pipe. */
	int rc = CVI_ISP_Run(pipe);
	printf("CVI_ISP_Run(%d) returned %#x\n", pipe, rc);
	return NULL;
}
static int ir_exposure_get(IrExposureReading *out)
{
    if(!out||dvp_selected||!state[0].isp||physical[0])return -1;
    ISP_EXPOSURE_ATTR_S attr={0};ISP_EXP_INFO_S info={0};
    int rc=CVI_ISP_GetExposureAttr(0,&attr);
    if(!rc)rc=CVI_ISP_QueryExposureInfo(0,&info);
    if(rc||attr.bByPass||attr.enOpType!=OP_TYPE_AUTO) {
        fprintf(stderr,"IR_AE read failed rc=%#x bypass=%d mode=%d\n",rc,attr.bByPass,attr.enOpType);return -1;
    }
    *out=(IrExposureReading){.target=attr.stAuto.u8Compensation,.time_us=info.u32ExpTime,
        .again=info.u32AGain,.dgain=info.u32DGain,.isp_gain=info.u32ISPDGain,.luma=info.u8AveLum,
        .stable=info.bIsStable,.sequence=atomic_load(&ir_capture_sequence)};
    return 0;
}
static int ir_exposure_set(int target)
{
    if(dvp_selected||!state[0].isp||physical[0]||target<1||target>255)return -1;
    ISP_EXPOSURE_ATTR_S attr={0},check={0};
    int rc=CVI_ISP_GetExposureAttr(0,&attr);
    if(rc||attr.bByPass||attr.enOpType!=OP_TYPE_AUTO)return -1;
    /* Preserve exposure route, gains, flicker policy, RGB ISP and lamps.
     * The SDK documents Compensation as AE's target luminance (not ROI Y). */
    attr.stAuto.u8Compensation=target;
    rc=CVI_ISP_SetExposureAttr(0,&attr);
    if(!rc)rc=CVI_ISP_GetExposureAttr(0,&check);
    if(rc||check.stAuto.u8Compensation!=target) {
        fprintf(stderr,"IR_AE target verify failed rc=%#x wanted=%d got=%u\n",rc,target,check.stAuto.u8Compensation);return -1;
    }
    return 0;
}
const IrExposureBackend media_ir_exposure_backend={ir_exposure_get,ir_exposure_set};
int media_ir_sample(const VIDEO_FRAME_INFO_S *frame,IrExposureSample *sample)
{
    /* Called once per acquired IR frame, before scaling. Latest-frame queue
     * drops preserve image provenance. Repeated VI frames do not count. */
    static uint64_t last_pts;
    static uint32_t last_ref;
    *sample=(IrExposureSample){0};
    uint64_t pts=frame->stVFrame.u64PTS;uint32_t ref=frame->stVFrame.u32TimeRef;
    if((!pts&&!ref)||(pts==last_pts&&ref==last_ref))return -1;
    last_pts=pts;last_ref=ref;
    uint64_t sequence=atomic_fetch_add(&ir_capture_sequence,1)+1;
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    IrExposureReading r;
    if(ir_exposure_get(&r))return -1;
    *sample=(IrExposureSample){.sequence=sequence,.captured_ms=(uint64_t)t.tv_sec*1000+t.tv_nsec/1000000,
        .target=r.target,.stable=r.stable,.valid=1};
    return 0;
}

int media_system_start(void)
{
	TRY(CVI_SYS_Init()); system_up = 1;
	VI_VPSS_MODE_S mode = {0};
	for (int i = 0; i < VI_MAX_PIPE_NUM; ++i) mode.aenMode[i] = VI_OFFLINE_VPSS_OFFLINE;
	TRY(CVI_SYS_SetVIVPSSMode(&mode));
	VB_CONFIG_S vb = {.u32MaxPoolCnt = 1};
	vb.astCommPool[0].u32BlkSize = COMMON_GetPicBufferSize(1920, 1080,
		PIXEL_FORMAT_NV21, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	vb.astCommPool[0].u32BlkCnt = cameras * 4 + 2;
	vb.astCommPool[0].enRemapMode = VB_REMAP_MODE_CACHED;
	TRY(CVI_VB_SetConfig(&vb));
	TRY(CVI_VB_Init()); vb_up = 1;
	return 0;
}

static int pipeline_start(void)
{
	TRY(sensor_board_prepare());
	TRY(sns->pfnExpSensorCb(&expfn));
	for (int i = 0; i < cameras; ++i) {
		int rgb = physical[i];
		state[i].ae_lib.s32Id = state[i].awb_lib.s32Id = i;
		strcpy(state[i].ae_lib.acLibName, CVI_AE_LIB_NAME);
		strcpy(state[i].awb_lib.acLibName, CVI_AWB_LIB_NAME);
		RX_INIT_ATTR_S rx = {0};
		/* MAC0 receives RX0..2; MAC1 receives RX3..5 in dual mode. */
		rx.MipiDev = rgb ? 0 : 1; rx.MipiMode = 0xff;
		rx.stMclkAttr.bMclkEn = 1; rx.stMclkAttr.u8Mclk = rgb ? 0 : 1;
		for (int lane = 0; lane <= MIPI_LANE_NUM; ++lane) rx.as16LaneId[lane] = -1;
		rx.as16LaneId[0] = rgb ? 2 : 5;
		rx.as16LaneId[1] = rgb ? 1 : 4;
		rx.as16LaneId[2] = rgb ? 0 : 3;
		TRY(sns->pfnPatchRxAttr(i, &rx));
		ISP_INIT_ATTR_S initial = {0};
		TRY(sns->pfnSetInit(i, &initial));
		ISP_SNS_COMMBUS_U bus = {.s8I2cDev = 3};
		TRY(sns->pfnSetBusInfo(i, bus));
		sns->pfnPatchI2cAddr(i, rgb ? 0x3f : 0x37);
		TRY(sns->pfnRegisterCallback(i, &state[i].ae_lib, &state[i].awb_lib));
		state[i].registered = 1;
		ISP_CMOS_SENSOR_IMAGE_MODE_S image = {.u16Width = 1920, .u16Height = 1080,
			.f32Fps = 30, .u8LaneNum = 2};
		TRY(expfn.pfn_cmos_set_image_mode(i, &image));
		TRY(expfn.pfn_cmos_set_wdr_mode(i, WDR_MODE_NONE));
		SNS_COMBO_DEV_ATTR_S combo = {0};
		TRY(sns->pfnGetRxAttr(i, &combo));
		TRY(CVI_MIPI_SetMipiReset(rx.MipiDev, 1));
		TRY(CVI_MIPI_SetMipiAttr(i, &combo));
		TRY(CVI_MIPI_SetSensorClock(rx.MipiDev, 1)); state[i].clock = 1;
		printf("pipe%d=%s I2C3/0x%02x MCLK%d=24M lanes=%d,%d,%d,-1,-1,-1\n",
			i, rgb ? "RGB" : "IR", rgb ? 0x3f : 0x37, rgb ? 0 : 1,
			rx.as16LaneId[0], rx.as16LaneId[1], rx.as16LaneId[2]);
	}
	usleep(10000);
	TRY(sensor_reset_set(cameras == 2 || !physical[0], cameras == 2 || physical[0]));
	usleep(20000);
	for (int i = 0; i < cameras; ++i) {
		TRY(sns->pfnSnsProbe(i));
		int hi = sns->pfnReadReg(i, 0x03f0), lo = sns->pfnReadReg(i, 0x03f1);
		printf("pipe%d sensor ID=%02x%02x\n", i, hi, lo);
		if (hi != 0x20 || lo != 0x83) return -1;
		VI_DEV_ATTR_S dev = {.snrFps = 30, .stSize = {1920,1080},
			.enIntfMode = VI_MODE_MIPI, .enInputDataType = VI_DATA_TYPE_RGB,
			.enWorkMode = VI_WORK_MODE_1Multiplex, .enBayerFormat = BAYER_FORMAT_RG};
		VI_DEV_BIND_PIPE_S bind = {.u32Num = 1, .MipiDev = physical[i] ? 0 : 1};
		for (int p = 0; p < VI_MAX_PIPE_NUM; ++p) bind.PipeId[p] = p ? -1 : i;
		TRY(CVI_VI_SetDevAttr(i, &dev));
		TRY(CVI_VI_SetDevBindAttr(i, &bind));
		TRY(CVI_VI_EnableDev(i)); state[i].dev = 1;
		VI_PIPE_ATTR_S pipe = {.u32MaxW = 1920, .u32MaxH = 1080,
			.enPixFmt = PIXEL_FORMAT_RGB_BAYER_12BPP, .enBitWidth = DATA_BITWIDTH_12,
			.stFrameRate = {-1,-1}, .bNrEn = CVI_TRUE, .enCompressMode = COMPRESS_MODE_NONE};
		TRY(CVI_VI_CreatePipe(i, &pipe)); state[i].pipe = 1;
		TRY(CVI_VI_StartPipe(i)); state[i].started = 1;
		TRY(CVI_AE_Register(i, &state[i].ae_lib)); state[i].ae = 1;
		TRY(CVI_AWB_Register(i, &state[i].awb_lib)); state[i].awb = 1;
		ISP_BIND_ATTR_S ispbind = {.stAeLib = state[i].ae_lib, .stAwbLib = state[i].awb_lib};
		TRY(CVI_ISP_SetBindAttr(i, &ispbind));
		TRY(CVI_ISP_MemInit(i)); state[i].isp = 1;
		ISP_PUB_ATTR_S pub = {.stWndRect = {0,0,1920,1080}, .stSnsSize = {1920,1080},
			.f32FrameRate = 30, .enBayer = BAYER_RGGB, .enWDRMode = WDR_MODE_NONE,
			.u8LaneNum = 2};
		TRY(CVI_ISP_SetPubAttr(i, &pub));
		TRY(CVI_ISP_Init(i));
		/* The IR sensor has no visible-color meaning. Disable chroma at ISP
		 * output for pipe0 only; RGB AWB/CCM remains independently configured. */
		ISP_MONO_ATTR_S mono={.Enable=physical[i]?CVI_FALSE:CVI_TRUE,.UpdateInterval=1};
		TRY(CVI_ISP_SetMonoAttr(i,&mono));
		ISP_MONO_ATTR_S actual={0};
		TRY(CVI_ISP_GetMonoAttr(i,&actual));
		if(actual.Enable!=mono.Enable) return -1;
		printf("ISP pipe%d=%s mono=%d\n",i,physical[i]?"RGB":"IR",actual.Enable);
	}
	for (int i = 0; i < cameras; ++i) {
		TRY(pthread_create(&isp_thread[i], NULL, run_isp, (void *)(intptr_t)i));
		++thread_up;
	}
	for (int i = 0; i < cameras; ++i) {
		expfn.pfn_cmos_sensor_init(i);
		VI_CHN_ATTR_S channel = {.stSize = {1920,1080}, .enPixelFormat = PIXEL_FORMAT_NV21,
			.enDynamicRange = DYNAMIC_RANGE_SDR8, .enVideoFormat = VIDEO_FORMAT_LINEAR,
			.enCompressMode = COMPRESS_MODE_NONE, .u32Depth = 2, .u32BindVbPool = (unsigned int)-1};
		TRY(CVI_VI_SetChnAttr(i, 0, &channel));
		TRY(CVI_VI_EnableChn(i, 0)); state[i].channel = 1;
	}
	return 0;
}

static void pipeline_stop(void)
{
	for (int i = cameras - 1; i >= 0; --i) {
		if (state[i].channel) CVI_VI_DisableChn(i, 0);
		if (state[i].registered && state[i].clock) sns->pfnStandby(i);
		if (state[i].started) CVI_VI_StopPipe(i);
	}
	for (int i = 0; i < cameras; ++i) if (state[i].isp) CVI_ISP_Exit(i);
	for (int i = 0; i < thread_up; ++i) pthread_join(isp_thread[i], NULL);
	for (int i = cameras - 1; i >= 0; --i) {
		if (state[i].registered) sns->pfnUnRegisterCallback(i, &state[i].ae_lib, &state[i].awb_lib);
		if (state[i].ae) CVI_AE_UnRegister(i, &state[i].ae_lib);
		if (state[i].awb) CVI_AWB_UnRegister(i, &state[i].awb_lib);
		if (state[i].pipe) CVI_VI_DestroyPipe(i);
		if (state[i].dev) CVI_VI_DisableDev(i);
		if (state[i].clock) CVI_MIPI_SetSensorClock(physical[i] ? 0 : 1, 0);
	}
	sensor_board_release();
	memset(state,0,sizeof(state));thread_up=0;
}


int media_camera_start(enum camera_source source)
{
    dvp_selected=source==CAMERA_DVP;
    if(dvp_selected) return gc0328_start();
    preview_pipe=source==CAMERA_RGB?1:0;
    return pipeline_start();
}
int media_camera_get(VIDEO_FRAME_INFO_S *frame) { return CVI_VI_GetChnFrame(dvp_selected?0:preview_pipe,0,frame,100); }
int media_camera_select_preview(enum camera_source source)
{
    if(dvp_selected||source==CAMERA_DVP) return -1;
    preview_pipe=source==CAMERA_RGB?1:0;return 0;
}
int media_camera_release(VIDEO_FRAME_INFO_S *frame) { return CVI_VI_ReleaseChnFrame(dvp_selected?0:preview_pipe,0,frame); }
int media_camera_get_ir(VIDEO_FRAME_INFO_S *frame)
{
    if(dvp_selected) return -1;
    return CVI_VI_GetChnFrame(0,0,frame,100);
}
int media_camera_release_ir(VIDEO_FRAME_INFO_S *frame) { return CVI_VI_ReleaseChnFrame(0,0,frame); }
int media_camera_get_rgb(VIDEO_FRAME_INFO_S *frame)
{
    if(dvp_selected)return -1;
    return CVI_VI_GetChnFrame(1,0,frame,100);
}
int media_camera_release_rgb(VIDEO_FRAME_INFO_S *frame){return CVI_VI_ReleaseChnFrame(1,0,frame);}
void media_camera_stop(void)
{
    if(dvp_selected) gc0328_stop();else pipeline_stop();
}
void media_system_stop(void)
{
    if(vb_up) CVI_VB_Exit();
    if(system_up) CVI_SYS_Exit();
    vb_up=system_up=0;
}
