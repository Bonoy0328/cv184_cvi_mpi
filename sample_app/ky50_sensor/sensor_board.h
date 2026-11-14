#ifndef KY50_SENSOR_BOARD_H
#define KY50_SENSOR_BOARD_H
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int sensor_reset_fd = -1, sensor_pwdn_fd = -1;
static volatile uint32_t *sensor_mux;
static const unsigned int sensor_mux_off[] = {0, 0xc, 8, 0x10, 0x14, 0x18, 0x100, 0x1d0};
static uint32_t sensor_saved_mux[8];

static int sensor_gpio_request(const char *label, unsigned int a, unsigned int b)
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
		strcpy(req.consumer_label, "ky50-sensor");
		int rc = ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req);
		close(fd);
		return rc ? -1 : req.fd;
	}
	return -1;
}

static int sensor_reset_set(int ir, int rgb)
{
	struct gpiohandle_data val = {.values = {ir, rgb}};
	return ioctl(sensor_reset_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &val);
}

static int sensor_board_prepare(void)
{
	const unsigned int fn[] = {0, 0, 3, 3, 0, 0, 3, 3};
	sensor_reset_fd = sensor_gpio_request("3020000.gpio", 4, 2);
	sensor_pwdn_fd = sensor_gpio_request("3021000.gpio", 5, 23);
	if (sensor_reset_fd < 0 || sensor_pwdn_fd < 0) return -1;
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) return -1;
	void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x03001000);
	close(fd);
	if (map == MAP_FAILED) return -1;
	sensor_mux = map;
	for (unsigned int i = 0; i < 8; ++i) {
		sensor_saved_mux[i] = sensor_mux[sensor_mux_off[i] / 4];
		sensor_mux[sensor_mux_off[i] / 4] = (sensor_saved_mux[i] & ~7U) | fn[i];
		(void)sensor_mux[sensor_mux_off[i] / 4];
	}
	/* Both PWDN pins low; both resets held low until MCLK is running. */
	return 0;
}

static void sensor_board_release(void)
{
	if (sensor_reset_fd >= 0) sensor_reset_set(0, 0);
	if (sensor_mux) {
		for (unsigned int i = 0; i < 8; ++i)
			sensor_mux[sensor_mux_off[i] / 4] = sensor_saved_mux[i];
		munmap((void *)sensor_mux, 4096); sensor_mux = NULL;
	}
	if (sensor_reset_fd >= 0) close(sensor_reset_fd);
	if (sensor_pwdn_fd >= 0) close(sensor_pwdn_fd);
	sensor_reset_fd = sensor_pwdn_fd = -1;
}
#endif
