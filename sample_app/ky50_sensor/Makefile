SHELL = /bin/bash
PARAM_FILE := ../../Makefile.param
include $(PARAM_FILE)

.PHONY: all clean
SNS_DIR := $(BUILD_PATH)/media/SensorSupportList
SNS_SRC := $(SNS_DIR)/gcore_gc2083/gc2083_cmos.c $(SNS_DIR)/gcore_gc2083/gc2083_sensor_ctl.c $(SNS_DIR)/sensor_i2c/sensor_i2c.c
SNS_HDR := $(wildcard $(SNS_DIR)/gcore_gc2083/*.h) $(SNS_DIR)/sensor_i2c/sensor_i2c.h
all: ky50_sensor_probe ky50_sensor_capture

ky50_sensor_probe: probe.c
	$(CC) $(CFLAGS) -I$(MW_INC) -I$(KERNEL_INC) $< -o $@ $(ELFFLAGS) -static -lmipi -lsys -lpthread -lm

LCD_DEP := $(wildcard ../ky50_lcd/*.[ch])
ky50_sensor_capture: capture.c display.c display.h sensor_board.h $(SNS_SRC) $(SNS_HDR) $(LCD_DEP)
	$(CC) $(CFLAGS) -O2 -I$(MW_INC) -I$(ISP_INC) -I$(KERNEL_INC) -I$(SNS_DIR)/sensor_i2c capture.c display.c $(SNS_SRC) -o $@ $(ELFFLAGS) -static -Wl,--start-group -lvi -lmipi -lmipi_tx -lsys -lisp -lcvi_bin -lawb -lae -laf -lisp_algo -lvpss -lvo -lgdc -Wl,--end-group -lpthread -lm -latomic

clean:
	rm -f ky50_sensor_probe ky50_sensor_capture
