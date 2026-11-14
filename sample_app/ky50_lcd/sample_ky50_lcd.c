/* KY50 V1.0 / M050-0287-1B0 NFS bring-up, using existing CV184x drivers.
 * Board wiring: Doc/KY50-V1.0(1).pdf.
 * --vendor-test uses the supplier's ST7102 initialization and video timing.
 * --otp-test retains the earlier, insufficient standard-DCS experiment.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "cvi_comm_gfbg.h"
#include "cvi_mipi_tx.h"
#include "cvi_sys.h"
#include "cvi_vo.h"
#include "st7102_init.h"
#include "dsi_raw.h"

#define WIDTH 480U
#define HEIGHT 854U
#define PINMUX_BASE 0x03001000
#define PINMUX_SIZE 4096

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t pattern_index;
static volatile sig_atomic_t status_requested;
static int dsi_fd = -1, fb_fd = -1, backlight_fd = -1, reset_fd = -1;
static int vo_enabled, dsi_configured;
static void *pixels = MAP_FAILED;
static size_t pixels_size;
static volatile uint32_t *pinmux;
/* CV184x cv184x_reg_fmux_gpio.h and KY50 schematic pages 2/4/5/8. */
static const unsigned int mux_offsets[] = {0x01c, 0x1c8, 0x198};
static uint32_t saved_mux[3];

static int check(int result, const char *operation)
{
	if (result) {
		fprintf(stderr, "%s failed: rc=%#x errno=%d (%s)\n",
			operation, result, errno, strerror(errno));
		return -1;
	}
	return 0;
}

#include "vo_frame.h"

static void on_signal(int sig)
{
	(void)sig;
	stopping = 1;
}

static void next_pattern(int sig)
{
	(void)sig;
	pattern_index = (pattern_index + 1) % 6;
}

static void request_status(int sig)
{
	(void)sig;
	status_requested = 1;
}

static int gpio_set(int fd, unsigned char first, unsigned char second)
{
	struct gpiohandle_data data = {.values = {first, second}};
	return ioctl(fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
}

/* Match controller labels, not Linux's dynamically assigned global numbers. */
static int request_gpio(const char *label, unsigned int first, int second)
{
	for (int i = 0; i < 16; ++i) {
		char path[64];
		struct gpiochip_info chip = {0};
		struct gpiohandle_request request = {0};
		snprintf(path, sizeof(path), "/dev/gpiochip%d", i);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chip) || strcmp(chip.label, label)) {
			close(fd);
			continue;
		}
		request.lineoffsets[0] = first;
		request.lines = second < 0 ? 1 : 2;
		if (second >= 0)
			request.lineoffsets[1] = (unsigned int)second;
		request.flags = GPIOHANDLE_REQUEST_OUTPUT;
		snprintf(request.consumer_label, sizeof(request.consumer_label), "ky50-lcd");
		int rc = ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &request);
		close(fd);
		if (check(rc, "request GPIO lines"))
			return -1;
		printf("GPIO %s: offset %u", label, first);
		if (second >= 0)
			printf(", %d", second);
		putchar('\n');
		return request.fd;
	}

	fprintf(stderr, "GPIO controller %s not found\n", label);
	return -1;
}

static int board_prepare(void)
{
	backlight_fd = request_gpio("3020000.gpio", 7, -1);
	if (backlight_fd < 0)
		return -1;
	/* ST7102 TDDI: release TP reset too (J10 pin 6), LCM reset on pin 12. */
	reset_fd = request_gpio("3022000.gpio", 25, 18);
	if (reset_fd < 0)
		return -1;
	int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0) {
		perror("open /dev/mem");
		return -1;
	}
	void *map = mmap(NULL, PINMUX_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, PINMUX_BASE);
	close(fd);
	if (map == MAP_FAILED) {
		perror("mmap pinmux");
		return -1;
	}
	pinmux = map;
	for (size_t i = 0; i < 3; ++i) {
		saved_mux[i] = pinmux[mux_offsets[i] / 4];
		printf("pinmux %#x: %#x -> GPIO function 3\n",
			PINMUX_BASE + mux_offsets[i], saved_mux[i]);
		pinmux[mux_offsets[i] / 4] = (saved_mux[i] & ~7U) | 3U;
		(void)pinmux[mux_offsets[i] / 4];
	}
	usleep(20000);
	if (check(gpio_set(reset_fd, 1, 1), "release LCM/TP reset"))
		return -1;
	usleep(150000);
	return 0;
}

static int send_dcs(unsigned char command, int parameter, unsigned int delay_ms)
{
	unsigned char bytes[2] = {command, parameter < 0 ? 0 : (unsigned char)parameter};
	struct cmd_info_s cmd = {
		.devno = 0, .data_type = parameter < 0 ? 0x05 : 0x15,
		.cmd_size = parameter < 0 ? 1 : 2, .cmd = bytes
	};
	printf("DCS write %02x", command);
	if (parameter >= 0)
		printf(" %02x", parameter);

	int rc = mipi_tx_send_cmd(dsi_fd, &cmd);
	printf(": rc=%d\n", rc);
	if (check(rc, "DCS write"))
		return -1;
	usleep(delay_ms * 1000);
	return 0;
}

static int initialize_st7102(void)
{
	for (size_t i = 0; i < sizeof(st7102_init) / sizeof(st7102_init[0]); ++i) {
		const struct st7102_command *entry = &st7102_init[i];
		unsigned char bytes[49] = {entry->command};
		memcpy(bytes + 1, entry->parameters, entry->count);
		struct cmd_info_s cmd = {
			.devno = 0, .data_type = entry->count > 1 ? 0x39 : entry->count ? 0x15 : 0x05,
			.cmd_size = entry->count + 1, .cmd = bytes
		};
		if (stopping)
			return -1;
		/* The supplier table omits COLMOD. Match the RGB888 video stream
		 * explicitly before Sleep Out; otherwise this panel reads 0C=00.
		 */
		if (entry->command == 0x11 && send_dcs(0x3a, 0x77, 0))
			return -1;
		int rc = entry->count > 1 && getenv("KY50_DIAG_PAYLOAD_CRC") ?
			send_payload_crc(cmd.data_type, bytes, cmd.cmd_size) : mipi_tx_send_cmd(dsi_fd, &cmd);
		printf("ST7102 [%zu] cmd=%02x params=%u type=%02x delay=%ums rc=%d\n",
			i, entry->command, entry->count, cmd.data_type, entry->delay_ms, rc);
		if (check(rc, "ST7102 initialization"))
			return -1;
		if (entry->delay_ms)
			usleep(entry->delay_ms * 1000);
	}
	return 0;
}

static void read_status(const char *stage)
{
	const unsigned char regs[] = {0xda, 0xdb, 0xdc, 0x0a, 0x0c, 0x05, 0x0f, 0xb0, 0xb7, 0xbf};
	for (size_t i = 0; i < sizeof(regs); ++i) {
		unsigned char bytes[4];
		struct get_cmd_info_s cmd = {
			.devno = 0, .data_type = 0x06, .data_param = regs[i],
			.get_data_size = sizeof(bytes), .get_data = bytes
		};
		int rc = -1;
		/* This SDK can return 0 on BTA timeout without filling get_data.
		 * These ID/status registers are not expected to contain the sentinel.
		 * Reads are diagnostic: never claim success from an untouched buffer.
		 */
		for (int attempt = 0; attempt < 3; ++attempt) {
			memset(bytes, 0xa5, sizeof(bytes));
			rc = mipi_tx_recv_cmd(dsi_fd, &cmd);
			if (!rc && bytes[0] != 0xa5)
				break;
			usleep(10000);
		}
		if (rc || bytes[0] == 0xa5)
			printf("%s DCS read %02x: unavailable rc=%d (check dmesg for BTA errors)\n",
				stage, regs[i], rc);
		else
			printf("%s DCS read %02x: %02x\n", stage, regs[i], bytes[0]);
		usleep(10000);
	}
}

static int draw_character(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	cvi_fb_point origin = {0, 0};
	cvi_fb_size screen = {WIDTH, HEIGHT};
	cvi_fb_colorkey colorkey = {0};
	CVI_BOOL show = CVI_FALSE;
	/* 5x7 A, scaled to 80x112, centered at (240,427). */
	static const unsigned char glyph[] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
	const unsigned int scale = 16, x0 = (WIDTH - 5 * scale) / 2;
	const unsigned int y0 = (HEIGHT - 7 * scale) / 2;

	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb_fd < 0) {
		perror("open /dev/fb0");
		return -1;
	}
	if (check(ioctl(fb_fd, FBIOPUT_SHOW_GFBG, &show), "hide framebuffer") ||
	    check(ioctl(fb_fd, FBIOGET_VSCREENINFO, &var), "get framebuffer mode"))
		return -1;
	var.xres = var.xres_virtual = WIDTH;
	var.yres = var.yres_virtual = HEIGHT;
	var.xoffset = var.yoffset = 0;
	var.bits_per_pixel = 32;
	var.red = (struct fb_bitfield){16, 8, 0};
	var.green = (struct fb_bitfield){8, 8, 0};
	var.blue = (struct fb_bitfield){0, 8, 0};
	var.transp = (struct fb_bitfield){24, 8, 0};
	var.activate = FB_ACTIVATE_NOW;
	if (check(ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var), "set ARGB8888 480x854") ||
	    check(ioctl(fb_fd, FBIOGET_VSCREENINFO, &var), "read back framebuffer mode") ||
	    check(ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix), "get framebuffer memory"))
		return -1;
	if (var.xres != WIDTH || var.yres != HEIGHT || var.bits_per_pixel != 32 ||
	    fix.line_length < WIDTH * 4 || fix.smem_len < (size_t)fix.line_length * HEIGHT) {
		fprintf(stderr, "Unexpected framebuffer geometry/stride/memory\n");
		return -1;
	}
	pixels_size = (size_t)fix.line_length * HEIGHT;
	pixels = mmap(NULL, pixels_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (pixels == MAP_FAILED) {
		perror("mmap framebuffer");
		return -1;
	}
	for (unsigned int y = 0; y < HEIGHT; ++y) {
		uint32_t *row = (uint32_t *)((unsigned char *)pixels + y * fix.line_length);
		for (unsigned int x = 0; x < WIDTH; ++x) {
			int ink = x >= x0 && x < x0 + 5 * scale &&
				y >= y0 && y < y0 + 7 * scale &&
				(glyph[(y - y0) / scale] & (1U << (4 - (x - x0) / scale)));
			row[x] = ink ? 0xffffffff : 0xff000000;
		}
	}
	show = CVI_TRUE;
	if (check(ioctl(fb_fd, FBIOPUT_SCREEN_ORIGIN_GFBG, &origin), "set framebuffer origin") ||
	    check(ioctl(fb_fd, FBIOPUT_SCREEN_SIZE, &screen), "set framebuffer output size") ||
	    check(ioctl(fb_fd, FBIOPUT_COLORKEY_GFBG, &colorkey), "disable colorkey") ||
	    check(ioctl(fb_fd, FBIOPAN_DISPLAY, &var), "submit framebuffer") ||
	    check(ioctl(fb_fd, FBIOPUT_SHOW_GFBG, &show), "show framebuffer"))
		return -1;
	printf("Framebuffer submitted: A at (%u,%u), 80x112, stride=%u.\n",
		x0, y0, fix.line_length);
	return 0;
}

static void cleanup(void)
{
	if (backlight_fd >= 0)
		gpio_set(backlight_fd, 0, 0);
	if (fb_fd >= 0) {
		CVI_BOOL show = CVI_FALSE;
		ioctl(fb_fd, FBIOPUT_SHOW_GFBG, &show);
		if (pixels != MAP_FAILED)
			munmap(pixels, pixels_size);
		close(fb_fd);
	}
	release_video_frame();
	if (vo_enabled)
		CVI_VO_Disable(0);
	if (dsi_fd >= 0) {
		if (dsi_configured)
			mipi_tx_disable(dsi_fd);
		close(dsi_fd);
	}
	if (dsi_raw_regs)
		munmap((void *)dsi_raw_regs, 4096);
	if (reset_fd >= 0)
		gpio_set(reset_fd, 0, 0);
	if (pinmux) {
		for (size_t i = 0; i < 3; ++i) {
			unsigned int index = mux_offsets[i] / 4;
			pinmux[index] = (pinmux[index] & ~7U) | (saved_mux[i] & 7U);
		}
		munmap((void *)pinmux, PINMUX_SIZE);
	}
	if (reset_fd >= 0)
		close(reset_fd);
	if (backlight_fd >= 0)
		close(backlight_fd);
}

int main(int argc, char **argv)
{
	unsigned int hsa, hbp, hfp, vsa, vbp, vfp, fps, seconds = 60;
	char tail;
	int otp_test, vendor_test;
	struct combo_dev_cfg_s cfg = {
		.devno = 0,
		.lane_id = {MIPI_TX_LANE_0, MIPI_TX_LANE_1, MIPI_TX_LANE_CLK, -1, -1},
		.output_mode = OUTPUT_MODE_DSI_VIDEO, .video_mode = BURST_MODE,
		.output_format = OUT_FORMAT_RGB_24_BIT,
	};
	/* Confirmed on the KY50 panel: 6/32/1 and 8/45/12 jittered with a
	 * dashed red line; 4/16/8 gives a stable centered A without the line.
	 */
	struct hs_settle_s hs = {.prepare = 4, .zero = 16, .trail = 8};
	VO_PUB_ATTR_S pub = {0};
	const char *diag_pn = getenv("KY50_DIAG_PN");
	const char *diag_hs = getenv("KY50_DIAG_HS");
	int diagnose = getenv("KY50_DIAG") != NULL;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 3 || argc > 4 ||
	    (strcmp(argv[1], "--probe") && strcmp(argv[1], "--otp-test") &&
	     strcmp(argv[1], "--vendor-test")) ||
	    sscanf(argv[2], "%u,%u,%u,%u,%u,%u,%u%c",
		&hsa, &hbp, &hfp, &vsa, &vbp, &vfp, &fps, &tail) != 7 ||
	    !hsa || !vsa || !fps || hsa > 1024 || hbp > 1024 || hfp > 1024 ||
	    vsa > 1024 || vbp > 1024 || vfp > 1024 || fps > 120 ||
	    (argc == 4 && (sscanf(argv[3], "%u%c", &seconds, &tail) != 1 || seconds > 86400))) {
		fprintf(stderr, "Usage: %s --probe|--otp-test|--vendor-test HSA,HBP,HFP,VSA,VBP,VFP,FPS [seconds]\n"
			"KY50 only, M050-0287-1B0 480x854, 2 lanes. seconds=0 waits for Ctrl-C.\n"
			"Supplier timing: 2,40,40,2,9,193,60. --vendor-test loads the supplier table.\n"
			"--otp-test only sends standard DCS; it did not establish visible output.\n", argv[0]);
		return 2;
	}
	otp_test = !strcmp(argv[1], "--otp-test");
	vendor_test = !strcmp(argv[1], "--vendor-test");
	if (diag_pn) {
		unsigned int mask;
		if (sscanf(diag_pn, "%u%c", &mask, &tail) != 1 || mask > 3)
			return 2;
		cfg.lane_pn_swap[1] = (mask & 1) != 0;
		cfg.lane_pn_swap[2] = (mask & 2) != 0;
		printf("Diagnostic P/N swap: data1=%u clock=%u\n",
			cfg.lane_pn_swap[1], cfg.lane_pn_swap[2]);
	}
	if (diag_hs) {
		unsigned int prepare, zero, trail;
		if (sscanf(diag_hs, "%u,%u,%u%c", &prepare, &zero, &trail, &tail) != 3 ||
		    prepare > 255 || zero > 255 || trail > 255)
			return 2;
		hs = (struct hs_settle_s){.prepare = prepare, .zero = zero, .trail = trail};
		printf("Diagnostic HS timing: %u,%u,%u\n", prepare, zero, trail);
	}
	int lock_fd = open("/tmp/ky50-lcd.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB)) {
		fprintf(stderr, "Cannot lock test; another ky50 LCD test may be running\n");
		return 1;
	}
	atexit(cleanup);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGUSR1, next_pattern);
	signal(SIGUSR2, request_status);
	cfg.sync_info = (struct sync_info_s){
		.vid_hsa_pixels = hsa, .vid_hbp_pixels = hbp, .vid_hfp_pixels = hfp,
		.vid_hline_pixels = WIDTH, .vid_vsa_lines = vsa, .vid_vbp_lines = vbp,
		.vid_vfp_lines = vfp, .vid_active_lines = HEIGHT,
	};
	cfg.pixel_clk = (WIDTH + hsa + hbp + hfp) * (HEIGHT + vsa + vbp + vfp) * fps / 1000;
	printf("KY50 %s: 480x854, RGB888, 2 lanes, pixel clock %u kHz\n",
		vendor_test ? "supplier initialization test" : otp_test ? "OTP experiment" : "DCS probe",
		cfg.pixel_clk);
	printf("Timing %u,%u,%u,%u,%u,%u,%u; LCD reset C25, touch reset C18.\n",
		hsa, hbp, hfp, vsa, vbp, vfp, fps);
	printf("HS prepare/zero/trail: %u/%u/%u\n", hs.prepare, hs.zero, hs.trail);
	if (check(CVI_SYS_Init(), "CVI_SYS_Init") || board_prepare())
		return 1;
	dsi_fd = open(MIPI_TX_NAME, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (dsi_fd < 0) {
		perror("open MIPI TX");
		return 1;
	}
	if (check(mipi_tx_disable(dsi_fd), "MIPI disable"))
		return 1;
	dsi_configured = 1;
	if (check(mipi_tx_cfg(dsi_fd, &cfg), "MIPI configure") ||
	    check(mipi_tx_set_hs_settle(dsi_fd, &hs), "MIPI HS timing") || diagnostic_clock_phase())
		return 1;
	/* The middleware can extend HFP to fit burst-mode packet overhead.
	 * Keep VO at the actual transmitter timing instead of overwriting it.
	 */
	if (check(ioctl(dsi_fd, MIPI_TX_GET_DEV_CFG, &cfg), "read actual MIPI timing"))
		return 1;
	hsa = cfg.sync_info.vid_hsa_pixels;
	hbp = cfg.sync_info.vid_hbp_pixels;
	hfp = cfg.sync_info.vid_hfp_pixels;
	vsa = cfg.sync_info.vid_vsa_lines;
	vbp = cfg.sync_info.vid_vbp_lines;
	vfp = cfg.sync_info.vid_vfp_lines;
	printf("Actual MIPI timing %u,%u,%u,%u,%u,%u; pixel clock %u kHz\n",
		hsa, hbp, hfp, vsa, vbp, vfp, cfg.pixel_clk);
	read_status("reset");
	if (!otp_test && !vendor_test)
		return 0;
	if (vendor_test ? initialize_st7102() :
	    (send_dcs(0x11, -1, 150) || send_dcs(0x3a, 0x77, 0) || send_dcs(0x29, -1, 50)))
		return 1;
	if (getenv("KY50_DIAG_POST_INIT_READ"))
		read_status("awake");
	pub.enIntfType = VO_INTF_MIPI;
	pub.enIntfSync = VO_OUTPUT_USER;
	pub.stSyncInfo = (VO_SYNC_INFO_S){
		.bSynm = CVI_TRUE, .bIop = CVI_TRUE, .u16FrameRate = fps,
		.u16Vact = HEIGHT, .u16Vbb = vbp, .u16Vfb = vfp,
		.u16Hact = WIDTH, .u16Hbb = hbp, .u16Hfb = hfp,
		.u16Hpw = hsa, .u16Vpw = vsa, .bIhs = CVI_TRUE, .bIvs = CVI_TRUE,
	};
	if (check(CVI_VO_SetPubAttr(0, &pub), "VO SetPubAttr") ||
	    check(mipi_tx_enable(dsi_fd), "MIPI enable") ||
	    check(CVI_VO_Enable(0), "VO enable"))
		return 1;
	vo_enabled = 1;
	VO_VIDEO_LAYER_ATTR_S layer = {
		.stDispRect = {0, 0, WIDTH, HEIGHT}, .stImageSize = {WIDTH, HEIGHT},
		.u32DispFrmRt = fps, .enPixFormat = PIXEL_FORMAT_RGB_888,
	};
	if (check(CVI_VO_SetVideoLayerAttr(0, &layer), "set full-size display layer"))
		return 1;
	if (check(CVI_VO_ShowPattern(0, VO_PAT_OFF), "disable hardware pattern") ||
	    (getenv("KY50_DIAG_GFBG") ? draw_character() : submit_video_character()) ||
	    check(gpio_set(backlight_fd, 1, 0), "enable LCD backlight"))
		return 1;
	printf("Test active; check the physical screen for a centered white A on black.\n"
		"SIGUSR1 cycles: A -> hardware colorbar -> red -> green -> blue -> black -> A.\n");
	int shown_pattern = 0;
	for (unsigned int elapsed = 0; !stopping && (!seconds || elapsed < seconds); ++elapsed) {
		sleep(1);
		if (diagnose && elapsed == 1)
			status_requested = 1;
		if (status_requested && !stopping) {
			status_requested = 0;
			if (check(mipi_tx_disable(dsi_fd), "pause MIPI for diagnostics"))
				return 1;
			read_status("after-video");
			if (check(mipi_tx_enable(dsi_fd), "resume MIPI after diagnostics"))
				return 1;
		}
		if (shown_pattern != pattern_index) {
			static const VO_PATTERN_MODE modes[] = {
				VO_PAT_OFF, VO_PAT_COLORBAR, VO_PAT_RED, VO_PAT_GREEN, VO_PAT_BLUE, VO_PAT_BLACK
			};
			static const char *names[] = {"centered A", "hardware colorbar", "red", "green", "blue", "black"};
			shown_pattern = pattern_index;
			CVI_BOOL show = shown_pattern ? CVI_FALSE : CVI_TRUE;
			if ((fb_fd >= 0 && check(ioctl(fb_fd, FBIOPUT_SHOW_GFBG, &show), "change graphics visibility")) ||
			    check(CVI_VO_ShowPattern(0, modes[shown_pattern]), "change hardware pattern"))
				return 1;
			printf("Visible test requested: %s\n", names[shown_pattern]);
		}
	}
	return 0;
}
