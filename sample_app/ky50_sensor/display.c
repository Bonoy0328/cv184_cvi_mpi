/* Reuse the physical-panel-verified ST7102 transport and GPIO implementation.
 * Keep the original character test and its timings unchanged.
 */
#define main ky50_character_test_main
#include "../ky50_lcd/sample_ky50_lcd.c"
#undef main
#include "display.h"

static int display_lock = -1;
static unsigned char *canvas;
static VIDEO_FRAME_INFO_S output;
static VB_BLK output_block = VB_INVALID_HANDLE;
enum { OUT_STRIDE = 1472, OUT_SIZE = OUT_STRIDE * HEIGHT };

int camera_display_start(void)
{
	display_lock = open("/tmp/ky50-lcd.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (display_lock < 0 || flock(display_lock, LOCK_EX | LOCK_NB)) return -1;
	setenv("KY50_DIAG_PAYLOAD_CRC", "1", 1);
	if (board_prepare()) return -1;
	struct combo_dev_cfg_s cfg = {
		.devno = 0,
		.lane_id = {MIPI_TX_LANE_0, MIPI_TX_LANE_1, MIPI_TX_LANE_CLK, -1, -1},
		.output_mode = OUTPUT_MODE_DSI_VIDEO, .video_mode = BURST_MODE,
		.output_format = OUT_FORMAT_RGB_24_BIT,
		.sync_info = {.vid_hsa_pixels = 2, .vid_hbp_pixels = 40, .vid_hfp_pixels = 40,
			.vid_hline_pixels = WIDTH, .vid_vsa_lines = 2, .vid_vbp_lines = 9,
			.vid_vfp_lines = 193, .vid_active_lines = HEIGHT},
		.pixel_clk = (WIDTH + 2 + 40 + 40) * (HEIGHT + 2 + 9 + 193) * 60 / 1000,
	};
	struct hs_settle_s hs = {.prepare = 4, .zero = 16, .trail = 8};
	dsi_fd = open(MIPI_TX_NAME, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (dsi_fd < 0 || check(mipi_tx_disable(dsi_fd), "MIPI disable")) return -1;
	dsi_configured = 1;
	if (check(mipi_tx_cfg(dsi_fd, &cfg), "MIPI configure") ||
	    check(mipi_tx_set_hs_settle(dsi_fd, &hs), "HS 4/16/8") ||
	    check(ioctl(dsi_fd, MIPI_TX_GET_DEV_CFG, &cfg), "actual MIPI timing") ||
	    initialize_st7102()) return -1;
	VO_PUB_ATTR_S pub = {.enIntfType = VO_INTF_MIPI, .enIntfSync = VO_OUTPUT_USER,
		.stSyncInfo = {.bSynm = CVI_TRUE, .bIop = CVI_TRUE, .u16FrameRate = 60,
			.u16Vact = HEIGHT, .u16Vbb = cfg.sync_info.vid_vbp_lines, .u16Vfb = cfg.sync_info.vid_vfp_lines,
			.u16Hact = WIDTH, .u16Hbb = cfg.sync_info.vid_hbp_pixels, .u16Hfb = cfg.sync_info.vid_hfp_pixels,
			.u16Hpw = cfg.sync_info.vid_hsa_pixels, .u16Vpw = cfg.sync_info.vid_vsa_lines,
			.bIhs = CVI_TRUE, .bIvs = CVI_TRUE}};
	if (check(CVI_VO_SetPubAttr(0, &pub), "VO attributes") ||
	    check(mipi_tx_enable(dsi_fd), "MIPI enable") || check(CVI_VO_Enable(0), "VO enable")) return -1;
	vo_enabled = 1;
	VO_VIDEO_LAYER_ATTR_S layer = {.stDispRect = {0,0,WIDTH,HEIGHT}, .stImageSize = {WIDTH,HEIGHT},
		.u32DispFrmRt = 60, .enPixFormat = PIXEL_FORMAT_RGB_888};
	if (check(CVI_VO_SetVideoLayerAttr(0, &layer), "display layer") ||
	    check(CVI_VO_ShowPattern(0, VO_PAT_OFF), "disable pattern") ||
	    check(CVI_VO_EnableVideoLayer(0), "enable layer")) return -1;
	video_layer_enabled = 1;
	VO_CHN_ATTR_S chn = {.stRect = {0,0,WIDTH,HEIGHT}};
	if (check(CVI_VO_SetChnAttr(0,0,&chn), "display channel") ||
	    check(CVI_VO_EnableChn(0,0), "enable channel")) return -1;
	video_channel_enabled = 1;
	VB_POOL_CONFIG_S pool = {.u32BlkSize = OUT_SIZE, .u32BlkCnt = 3,
		.enRemapMode = VB_REMAP_MODE_CACHED, .acName = "ky50-camera-display"};
	video_pool = CVI_VB_CreatePool(&pool);
	if (video_pool == VB_INVALID_POOLID) return -1;
	if (gpio_set(backlight_fd, 1, 0)) return -1;
	printf("DISPLAY 480x854 RGB888 stride=1472 HS=4/16/8 clock=%u; top=IR bottom=RGB\n", cfg.pixel_clk);
	return 0;
}

int camera_display_begin(void)
{
	output_block = CVI_VB_GetBlock(video_pool, OUT_SIZE);
	if (output_block == VB_INVALID_HANDLE) return -1;
	memset(&output, 0, sizeof(output));
	output.u32PoolId = video_pool;
	output.stVFrame = (VIDEO_FRAME_S){.u32Width = WIDTH, .u32Height = HEIGHT,
		.u32Stride = {OUT_STRIDE,0,0}, .u32Length = {OUT_SIZE,0,0},
		.u64PhyAddr = {CVI_VB_Handle2PhysAddr(output_block),0,0},
		.enPixelFormat = PIXEL_FORMAT_RGB_888, .enVideoFormat = VIDEO_FORMAT_LINEAR,
		.enCompressMode = COMPRESS_MODE_NONE, .enDynamicRange = DYNAMIC_RANGE_SDR8,
		.enColorGamut = COLOR_GAMUT_BT601};
	canvas = CVI_SYS_MmapCache(output.stVFrame.u64PhyAddr[0], OUT_SIZE);
	if (!canvas || canvas == MAP_FAILED) { canvas = NULL; return -1; }
	memset(canvas, 0, OUT_SIZE);
	return 0;
}

static unsigned char clip(int value) { return value < 0 ? 0 : value > 255 ? 255 : value; }

int camera_display_add(const VIDEO_FRAME_INFO_S *frame, int rgb)
{
	const VIDEO_FRAME_S *v = &frame->stVFrame;
	unsigned char *src[2] = {NULL,NULL};
	unsigned int size[2] = {v->u32Stride[0] * 1080, v->u32Stride[1] * 540};
	int rc = -1;
	if (!canvas || v->u32Width != 1920 || v->u32Height != 1080 ||
	    v->enPixelFormat != PIXEL_FORMAT_NV21 || v->enCompressMode != COMPRESS_MODE_NONE) return -1;
	for (int p = 0; p < 2; ++p) {
		if (v->u32Length[p] < size[p]) goto out;
		src[p] = CVI_SYS_MmapCache(v->u64PhyAddr[p], size[p]);
		if (!src[p] || src[p] == MAP_FAILED) { src[p] = NULL; goto out; }
		if (CVI_SYS_IonInvalidateCache(v->u64PhyAddr[p], src[p], size[p])) goto out;
	}
	unsigned int top = rgb ? 506 : 79;
	for (unsigned int y = 0; y < 270; ++y) {
		unsigned char *row = canvas + (top + y) * OUT_STRIDE;
		const unsigned char *lum = src[0] + y * 4 * v->u32Stride[0];
		const unsigned char *vu = src[1] + y * 2 * v->u32Stride[1];
		for (unsigned int x = 0; x < WIDTH; ++x) {
			int luma = 298 * ((int)lum[x * 4] - 16);
			int u = rgb ? (int)vu[x * 4 + 1] - 128 : 0;
			int vval = rgb ? (int)vu[x * 4] - 128 : 0;
			row[x*3] = clip((luma + 409*vval + 128) >> 8);
			row[x*3+1] = clip((luma - 100*u - 208*vval + 128) >> 8);
			row[x*3+2] = clip((luma + 516*u + 128) >> 8);
		}
	}
	output.stVFrame.u64PTS = v->u64PTS;
	rc = 0;
out:
	for (int p = 0; p < 2; ++p) if (src[p]) CVI_SYS_Munmap(src[p], size[p]);
	return rc;
}

int camera_display_end(void)
{
	if (!canvas) return -1;
	int rc = CVI_SYS_IonFlushCache(output.stVFrame.u64PhyAddr[0], canvas, OUT_SIZE);
	CVI_SYS_Munmap(canvas, OUT_SIZE); canvas = NULL;
	if (!rc) rc = CVI_VO_SendFrame(0, 0, &output, 1000);
	int release_rc = CVI_VB_ReleaseBlock(output_block);
	output_block = VB_INVALID_HANDLE;
	return rc ? rc : release_rc;
}

void camera_display_stop(void)
{
	if (canvas) { CVI_SYS_Munmap(canvas, OUT_SIZE); canvas = NULL; }
	if (output_block != VB_INVALID_HANDLE) CVI_VB_ReleaseBlock(output_block);
	cleanup(); /* video_buffers_initialized is zero: capture owns VB_Init/Exit. */
	if (display_lock >= 0) close(display_lock);
}
