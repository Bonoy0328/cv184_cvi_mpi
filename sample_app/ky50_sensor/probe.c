/* KY50 schematic pages 2, 5, 7. Controlled ID-only test, no register init. */
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>
#include "cvi_mipi.h"

static int lines(const char *label, unsigned int a, unsigned int b)
{
	for (int i = 0; i < 16; ++i) {
		char path[40];
		struct gpiochip_info chip = {0};
		struct gpiohandle_request req = {0};
		snprintf(path, sizeof(path), "/dev/gpiochip%d", i);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) continue;
		if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chip) || strcmp(label, chip.label)) {
			close(fd); continue;
		}
		req.lineoffsets[0] = a; req.lineoffsets[1] = b;
		req.lines = 2; req.flags = GPIOHANDLE_REQUEST_OUTPUT;
		strcpy(req.consumer_label, "ky50-sensor-probe");
		int rc = ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req);
		close(fd);
		return rc ? -1 : req.fd;
	}
	return -1;
}

static int set(int fd, int a, int b)
{
	struct gpiohandle_data val = {.values = {a, b}};
	return ioctl(fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &val);
}

static int read_id_byte(int fd, unsigned int addr, unsigned int reg)
{
	unsigned char index[2] = {reg >> 8, reg & 255}, val = 0xa5;
	struct i2c_msg msg[2] = {
		{.addr = addr, .len = 2, .buf = index},
		{.addr = addr, .flags = I2C_M_RD, .len = 1, .buf = &val}
	};
	struct i2c_rdwr_ioctl_data data = {.msgs = msg, .nmsgs = 2};
	if (ioctl(fd, I2C_RDWR, &data) != 2) return -1;
	return val;
}

int main(int argc, char **argv)
{
	/* Select IR/RGB/both. PWDN level is explicit until module wiring is proven. */
	if (argc != 3 || (strcmp(argv[1], "ir") && strcmp(argv[1], "rgb") && strcmp(argv[1], "both")) ||
	    (strcmp(argv[2], "0") && strcmp(argv[2], "1"))) {
		fprintf(stderr, "usage: %s ir|rgb|both pwdn-level(0|1)\n", argv[0]); return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	int lock = open("/tmp/ky50-sensor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
		fprintf(stderr, "Sensor resources are in use; stop the capture program first.\n");
		if (lock >= 0) close(lock);
		return 1;
	}
	int rc = 1, reset = -1, pwdn = -1, bus = -1, mem = -1;
	int clock_enabled[2] = {0, 0};
	volatile uint32_t *mux = MAP_FAILED;
	const unsigned int off[] = {0x000, 0x00c, 0x008, 0x010, 0x014, 0x018, 0x100, 0x1d0};
	const unsigned int fun[] = {0, 0, 3, 3, 0, 0, 3, 3};
	uint32_t saved[8];
	int active[] = {strcmp(argv[1], "rgb") != 0, strcmp(argv[1], "ir") != 0};
	reset = lines("3020000.gpio", 4, 2);
	pwdn = lines("3021000.gpio", 5, 23);
	if (reset < 0 || pwdn < 0) { perror("request sensor GPIO"); goto out; }
	mem = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem < 0) goto out;
	mux = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem, 0x03001000);
	if (mux == MAP_FAILED) goto out;
	for (unsigned int i = 0; i < 8; ++i) {
		saved[i] = mux[off[i] / 4];
		printf("mux %#x: %#x -> %u\n", 0x03001000 + off[i], saved[i], fun[i]);
		mux[off[i] / 4] = (saved[i] & ~7U) | fun[i];
		(void)mux[off[i] / 4];
	}
	if (set(pwdn, atoi(argv[2]), atoi(argv[2]))) goto out;
	for (int i = 0; i < 2; ++i) {
		struct combo_dev_attr_s attr = {
			.input_mode = INPUT_MODE_MIPI, .mac_clk = RX_MAC_CLK_200M,
			.mclk = {.cam = i ? 0 : 1, .freq = CAMPLL_FREQ_24M},
			.mipi_attr = {.raw_data_type = RAW_DATA_10BIT,
				.lane_id = {i ? 2 : 5, i ? 1 : 4, i ? 0 : 3, -1, -1, -1},
				.wdr_mode = CVI_MIPI_WDR_MODE_NONE},
			.devno = i ? 0 : 1, .cif_mode = 0xff,
			.img_size = {.width = 1920, .height = 1080, .active_w = 1920, .active_h = 1080,
				.max_width = 1920, .max_height = 1080}
		};
		int ret = CVI_MIPI_SetMipiAttr(i, &attr);
		printf("MIPI dev%u attr rc=%#x\n", attr.devno, ret);
		if (ret || CVI_MIPI_SetSensorClock(attr.devno, 1)) goto out;
		clock_enabled[attr.devno] = 1;
	}
	usleep(10000);
	if (set(reset, active[0], active[1])) goto out;
	usleep(20000);
	bus = open("/dev/i2c-3", O_RDWR);
	if (bus < 0) goto out;
	printf("ID test: %s RESET=(%d,%d) PWDN=%s MCLK=(24M,24M)\n",
		argv[1], active[0], active[1], argv[2]);
	for (unsigned int addr = 0x37; addr <= 0x3f; addr += 8) {
		int hi = read_id_byte(bus, addr, 0x03f0), lo = read_id_byte(bus, addr, 0x03f1);
		printf("I2C3 address=0x%02x ID_H=%d ID_L=%d ID=0x%04x %s\n",
			addr, hi, lo, hi < 0 || lo < 0 ? 0xffff : (hi << 8) | lo,
			hi == 0x20 && lo == 0x83 ? "GC2083 confirmed" : "no valid GC2083 ID");
		if (hi == 0x20 && lo == 0x83) rc = 0;
	}
out:
	if (reset >= 0) set(reset, 0, 0);
	for (int i = 0; i < 2; ++i) if (clock_enabled[i]) CVI_MIPI_SetSensorClock(i, 0);
	if (mux != MAP_FAILED) {
		for (unsigned int i = 0; i < 8; ++i) mux[off[i] / 4] = saved[i];
		munmap((void *)mux, 4096);
	}
	if (bus >= 0) close(bus);
	if (mem >= 0) close(mem);
	if (reset >= 0) close(reset);
	if (pwdn >= 0) close(pwdn);
	close(lock);
	return rc;
}
