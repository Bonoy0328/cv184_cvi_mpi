SHELL = /bin/bash
ifeq ($(PARAM_FILE), )
PARAM_FILE := ../../Makefile.param
include $(PARAM_FILE)
endif

TARGET = sample_ky50_lcd
.PHONY: all clean
all: $(TARGET)

$(TARGET): sample_ky50_lcd.c st7102_init.h dsi_raw.h vo_frame.h $(MW_LIB)/libvo.a $(MW_LIB)/libmipi_tx.a $(MW_LIB)/libsys.a
	$(CC) $(CFLAGS) -I$(MW_INC) $< -o $@ $(ELFFLAGS) -static -lvo -lmipi_tx -lsys -lpthread -lm

clean:
	rm -f $(TARGET)
