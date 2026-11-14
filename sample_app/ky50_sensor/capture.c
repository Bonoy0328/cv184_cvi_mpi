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
#include "sensor_board.h"
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
#include "display.h"

extern ISP_SNS_OBJ_S stSnsGc2083_Obj;
static ISP_SNS_OBJ_S *sns = &stSnsGc2083_Obj;
static ISP_SENSOR_EXP_FUNC_S expfn;
static volatile sig_atomic_t stop_requested;
static int cameras, physical[2], system_up, vb_up, thread_up;
static pthread_t isp_thread[2];
static struct {
	int registered, clock, dev, pipe, started, ae, awb, isp, channel;
	ALG_LIB_S ae_lib, awb_lib;
} state[2];

#define TRY(call) do { int result = (call); if (result) { \
	fprintf(stderr, "%s failed: %#x (line %d)\n", #call, result, __LINE__); return -1; \
} } while (0)

static void stop_signal(int sig) { (void)sig; stop_requested = 1; }
static void *run_isp(void *arg)
{
	int pipe = (int)(intptr_t)arg;
	/* Linux ISP shares an event loop, but Run must enable each pipe. */
	int rc = CVI_ISP_Run(pipe);
	printf("CVI_ISP_Run(%d) returned %#x\n", pipe, rc);
	return NULL;
}

static int pipeline_start(void)
{
	TRY(CVI_SYS_Init()); system_up = 1;
	TRY(sensor_board_prepare());
	TRY(sns->pfnExpSensorCb(&expfn));
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
	if (vb_up) CVI_VB_Exit();
	if (system_up) CVI_SYS_Exit();
	sensor_board_release();
}

static int inspect_frame(int pipe, VIDEO_FRAME_INFO_S *frame, unsigned int count)
{
	VIDEO_FRAME_S *v = &frame->stVFrame;
	if (v->u32Width != 1920 || v->u32Height != 1080 || v->enPixelFormat != PIXEL_FORMAT_NV21 ||
	    v->enCompressMode != COMPRESS_MODE_NONE || v->u32Stride[0] < 1920 || v->u32Stride[1] < 1920)
		return -1;
	if (count != 1 && count != 90) return 0;
	char path[100];
	snprintf(path, sizeof(path), "%s-%03u-1920x1080.nv21", physical[pipe] ? "rgb" : "ir", count);
	FILE *file = fopen(path, "wb");
	if (!file) return -1;
	int rc = 0;
	for (int p = 0; p < 2 && !rc; ++p) {
		unsigned int rows = p ? 540 : 1080;
		unsigned int size = v->u32Stride[p] * rows;
		if (v->u32Length[p] < size) { rc = -1; break; }
		unsigned char *map = CVI_SYS_MmapCache(v->u64PhyAddr[p], size);
		if (!map || map == MAP_FAILED) { rc = -1; break; }
		if (CVI_SYS_IonInvalidateCache(v->u64PhyAddr[p], map, size)) rc = -1;
		uint64_t sum = 0;
		for (unsigned int y = 0; y < rows && !rc; ++y) {
			unsigned char *row = map + y * v->u32Stride[p];
			if (fwrite(row, 1, 1920, file) != 1920) { rc = -1; break; }
			for (unsigned int x = 0; x < 1920; ++x) sum += row[x];
		}
		printf("%s plane%d stride=%u length=%u mean=%.2f\n", path, p,
			v->u32Stride[p], v->u32Length[p], (double)sum / (rows * 1920));
		CVI_SYS_Munmap(map, size);
	}
	if (fclose(file)) rc = -1;
	return rc;
}

/* Optional observation only: correlate AE output with actual image brightness. */
static void diagnose_frame(int pipe, VIDEO_FRAME_INFO_S *frame, unsigned int count)
{
	VIDEO_FRAME_S *v = &frame->stVFrame;
	ISP_EXP_INFO_S ae = {0};
	int ae_rc = CVI_ISP_QueryExposureInfo(pipe, &ae);
	unsigned int size = v->u32Stride[0] * v->u32Height;
	unsigned char *map = CVI_SYS_MmapCache(v->u64PhyAddr[0], size);
	double mean = -1.0;
	if (map && map != MAP_FAILED) {
		if (!CVI_SYS_IonInvalidateCache(v->u64PhyAddr[0], map, size)) {
			uint64_t sum = 0, samples = 0;
			for (unsigned int y = 0; y < v->u32Height; y += 4)
				for (unsigned int x = 0; x < v->u32Width; x += 4) {
					sum += map[y * v->u32Stride[0] + x];
					++samples;
				}
			if (samples) mean = (double)sum / samples;
		}
		CVI_SYS_Munmap(map, size);
	}
	printf("DIAG %s count=%u Ymean=%.2f AE_rc=%#x exp_us=%u again=%u dgain=%u isp_gain=%u AE_luma=%u stable=%d max=%d\n",
		physical[pipe] ? "RGB" : "IR", count, mean, ae_rc, ae.u32ExpTime,
		ae.u32AGain, ae.u32DGain, ae.u32ISPDGain, ae.u8AveLum, ae.bIsStable, ae.bExposureIsMAX);
}

int main(int argc, char **argv)
{
	if ((argc != 3 && argc != 4) || (argc == 4 && strcmp(argv[3], "--display")) ||
	    (strcmp(argv[1], "ir") && strcmp(argv[1], "rgb") && strcmp(argv[1], "both"))) {
		fprintf(stderr, "usage: %s ir|rgb|both frames(0=continuous) [--display]\n", argv[0]); return 2;
	}
	char *end;
	unsigned long limit = strtoul(argv[2], &end, 10);
	if (!*argv[2] || *end || limit > 1000000) return 2;
	cameras = !strcmp(argv[1], "both") ? 2 : 1;
	physical[0] = !strcmp(argv[1], "rgb"); physical[1] = 1;
	setvbuf(stdout, NULL, _IONBF, 0);
	int lock = open("/tmp/ky50-sensor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) return 1;
	signal(SIGINT, stop_signal); signal(SIGTERM, stop_signal); signal(SIGHUP, stop_signal);
	int rc = pipeline_start();
	int display = argc == 4;
	int diagnostic = getenv("KY50_SENSOR_DIAG") != NULL;
	int display_attempted = display && !rc;
	if (display_attempted) rc = camera_display_start();
	uint64_t first_pts[2] = {0}, last_pts[2] = {0};
	unsigned int count = 0;
	while (!rc && !stop_requested && (!limit || count < limit)) {
		if (display && (rc = camera_display_begin())) break;
		++count;
		for (int i = 0; i < cameras; ++i) {
			VIDEO_FRAME_INFO_S frame = {0};
			rc = CVI_VI_GetChnFrame(i, 0, &frame, 10000);
			if (rc) {
				fprintf(stderr, "GetChnFrame pipe%d #%u failed %#x stream=%#x\n",
					i, count, rc, sns->pfnReadReg(i, 0x003e));
				system("cat /proc/mipi-rx; cat /proc/soph/vi; cat /proc/soph/vi_dbg");
				break;
			}
			uint64_t pts = frame.stVFrame.u64PTS;
			if (count == 1) first_pts[i] = pts;
			if (count > 1 && pts <= last_pts[i]) rc = -1;
			last_pts[i] = pts;
			if (inspect_frame(i, &frame, count)) rc = -1;
			if (diagnostic && (count == 1 || count % 150 == 0)) diagnose_frame(i, &frame, count);
			if (display && camera_display_add(&frame, physical[i])) rc = -1;
			if (count == 1 || count % 30 == 0)
				printf("FRAME %s count=%u pts=%" PRIu64 " fps=%.2f format=%d compressed=%d\n",
					physical[i] ? "RGB" : "IR", count, pts,
					pts > first_pts[i] ? (count - 1) * 1000000.0 / (pts - first_pts[i]) : 0.0,
					frame.stVFrame.enPixelFormat, frame.stVFrame.enCompressMode);
			if (CVI_VI_ReleaseChnFrame(i, 0, &frame)) rc = -1;
			if (rc) break;
		}
		if (display && !rc) rc = camera_display_end();
	}
	printf("Capture finished count=%u rc=%#x\n", count, rc);
	if (display_attempted) camera_display_stop();
	pipeline_stop();
	close(lock);
	return rc ? 1 : 0;
}
