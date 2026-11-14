/* Submit the A as a complete video frame, so the
 * display DMA has valid backing memory independently of the graphics layer.
 */
#ifndef KY50_VO_FRAME_H
#define KY50_VO_FRAME_H

#include "cvi_vb.h"

static VB_POOL video_pool = VB_INVALID_POOLID;
static VB_BLK video_block = VB_INVALID_HANDLE;
static int video_buffers_initialized, video_layer_enabled, video_channel_enabled;

static void release_video_frame(void)
{
	if (video_channel_enabled)
		CVI_VO_DisableChn(0, 0);
	if (video_layer_enabled)
		CVI_VO_DisableVideoLayer(0);
	if (video_block != VB_INVALID_HANDLE)
		CVI_VB_ReleaseBlock(video_block);
	if (video_pool != VB_INVALID_POOLID)
		CVI_VB_DestroyPool(video_pool);
	if (video_buffers_initialized)
		CVI_VB_Exit();
}

static int submit_video_character(void)
{
	const unsigned int stride = (WIDTH * 3 + 63) & ~63U;
	const unsigned int size = stride * HEIGHT;
	static const unsigned char glyph[] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
	const unsigned int scale = 16, x0 = (WIDTH - 5 * scale) / 2;
	const unsigned int y0 = (HEIGHT - 7 * scale) / 2;
	VB_POOL_CONFIG_S pool = {
		.u32BlkSize = size, .u32BlkCnt = 1,
		.enRemapMode = VB_REMAP_MODE_NOCACHE, .acName = "ky50-video-test"
	};
	VO_CHN_ATTR_S channel = {.stRect = {0, 0, WIDTH, HEIGHT}};
	VIDEO_FRAME_INFO_S frame = {0};
	if (check(CVI_VB_Init(), "initialize video buffers"))
		return -1;
	video_buffers_initialized = 1;
	video_pool = CVI_VB_CreatePool(&pool);
	if (video_pool == VB_INVALID_POOLID)
		return -1;
	video_block = CVI_VB_GetBlock(video_pool, size);
	if (video_block == VB_INVALID_HANDLE)
		return -1;
	frame.u32PoolId = video_pool;
	frame.stVFrame.u64PhyAddr[0] = CVI_VB_Handle2PhysAddr(video_block);
	frame.stVFrame.u32Stride[0] = stride;
	frame.stVFrame.u32Length[0] = size;
	frame.stVFrame.u32Width = WIDTH;
	frame.stVFrame.u32Height = HEIGHT;
	frame.stVFrame.enPixelFormat = PIXEL_FORMAT_RGB_888;
	frame.stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
	frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
	frame.stVFrame.enDynamicRange = DYNAMIC_RANGE_SDR8;
	frame.stVFrame.enColorGamut = COLOR_GAMUT_BT601;
	unsigned char *map = CVI_SYS_Mmap(frame.stVFrame.u64PhyAddr[0], size);
	if (!map || map == MAP_FAILED)
		return -1;
	memset(map, 0, size);
	for (unsigned int y = y0; y < y0 + 7 * scale; ++y)
		for (unsigned int x = x0; x < x0 + 5 * scale; ++x)
			if (glyph[(y - y0) / scale] & (1U << (4 - (x - x0) / scale)))
				memset(map + y * stride + x * 3, 0xff, 3);
	__sync_synchronize();
	CVI_SYS_Munmap(map, size);
	if (check(CVI_VO_EnableVideoLayer(0), "enable video layer"))
		return -1;
	video_layer_enabled = 1;
	if (check(CVI_VO_SetChnAttr(0, 0, &channel), "set video channel") ||
	    check(CVI_VO_EnableChn(0, 0), "enable video channel"))
		return -1;
	video_channel_enabled = 1;
	if (check(CVI_VO_SendFrame(0, 0, &frame, 1000), "submit video A"))
		return -1;
	printf("RGB video A submitted: 480x854 stride=%u, private pool=%u.\n", stride, video_pool);
	return 0;
}

#endif
