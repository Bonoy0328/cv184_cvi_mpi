SHELL := /bin/bash
SDK := $(abspath ../../..)
TOOL := $(SDK)/host-tools/gcc/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf/bin/arm-none-linux-musleabihf-
TARGET ?= board
ifeq ($(TARGET),host)
CC := gcc
LINK := $(CC)
BIN := ui_preview
APP := ui.c status.c fonts.c settings.c preview.c user_ui.c ime.c access.c access_store.c
LIBS := -lm -lpthread
else
CC := $(TOOL)gcc
CXX := $(TOOL)g++
LINK := $(CXX)
ARCH_FLAGS := -mcpu=cortex-a53 -mfpu=neon-vfpv4
BIN := ky50_ui
SNS := $(SDK)/build/media/SensorSupportList
SNS_SRC := $(SNS)/gcore_gc2083/gc2083_cmos.c $(SNS)/gcore_gc2083/gc2083_sensor_ctl.c $(SNS)/sensor_i2c/sensor_i2c.c
APP := ui.c status.c fonts.c input.c main.c palm_queue.c camera_port.c display_port.c scale.c pixels.c settings.c factory.c gc0328.c hand_overlay.c user_ui.c ime.c access.c access_store.c
CPP_APP := handpose_bridge.cpp
PVA_SOURCE := $(SDK)/libpva_face
PVA_LIBS := pva/lib/libpva.a pva/lib/libdeviceCerf.a pva/lib/libsodium.a
BM_LIB := $(SDK)/install/soc_cv1842hp_wevb_0014a_spinand/rootfs/system/lib
CPPFLAGS += -Ipva/include -I$(PVA_SOURCE)/third_party/nlohmann
CPPFLAGS += -I$(SDK)/cvi_mpi/include -I$(SDK)/cvi_mpi/include/isp -I$(SNS)/sensor_i2c
CPPFLAGS += -I$(SDK)/cvi_mpi/self_test/audio_ut/audio_pcm -I$(SDK)/linux_5.10/sound/soc/cvitek
CPPFLAGS += -DARCH_CV184X
CPPFLAGS += -I$(SDK)/linux_5.10/build/cv1842hp_wevb_0014a_spinand/arm/usr/include
LIBS := -static-libstdc++ -static-libgcc -Wl,-Bstatic -L$(SDK)/cvi_mpi/lib -Lpva/lib -Wl,--start-group -lvi -lmipi -lmipi_tx -lsys -lisp -lcvi_bin -lawb -lae -laf -lisp_algo -lvpss -lvo -lgdc -ltinyalsa -lpva -ldeviceCerf -lsodium -Wl,--end-group -Wl,-Bdynamic \
    -L$(BM_LIB) -lbmrt -lbmlib -lstdc++fs -lpthread -lm -latomic -ldl \
    -Wl,-rpath,/system/lib -Wl,-rpath-link,$(BM_LIB)
endif
APP += ir_exposure.c
CPPFLAGS += -I. -Ivendor/lvgl -Ivendor/lvgl/include -DLV_CONF_INCLUDE_SIMPLE -Ivendor/sqlite -Ipva/include
CFLAGS := -std=gnu11 -O2 -g -ffunction-sections -fdata-sections -Wall -Wextra $(ARCH_FLAGS)
LV_SRC := $(sort $(shell rg --files vendor/lvgl/src -g '*.c'))
OBJ := $(addprefix build/$(TARGET)/,$(APP:.c=.o) $(CPP_APP:.cpp=.o) $(LV_SRC:.c=.o)) build/$(TARGET)/vendor/sqlite/sqlite3.o
SNS_OBJ := $(addprefix build/$(TARGET)/sensor/,$(notdir $(SNS_SRC:.c=.o)))
.PHONY: all host test kernel clean
all: $(BIN)
$(BIN): $(OBJ) $(SNS_OBJ) $(PVA_LIBS)
	$(LINK) -Wl,--gc-sections $(OBJ) $(SNS_OBJ) $(LIBS) -o $@
build/$(TARGET)/%.o: %.c lv_conf.h Makefile
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
build/$(TARGET)/%.o: %.cpp Makefile
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) -std=c++11 -O2 -g -ffunction-sections -fdata-sections -Wall -Wextra -Werror $(ARCH_FLAGS) -MMD -MP -c $< -o $@
$(addprefix build/$(TARGET)/,$(APP:.c=.o)): CFLAGS += -Werror
build/$(TARGET)/vendor/sqlite/sqlite3.o: CPPFLAGS += -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0
build/board/pixels.o: CFLAGS += -mcpu=cortex-a53 -mfpu=neon-vfpv4
define sensor_rule
build/$(TARGET)/sensor/$(notdir $(1:.c=.o)): $(1) Makefile
	@mkdir -p $$(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $$< -o $$@
endef
$(foreach src,$(SNS_SRC),$(eval $(call sensor_rule,$(src))))
host:
	$(MAKE) TARGET=host
test: host overlay-test access-test exposure-test
	mkdir -p build/previews
	./ui_preview build/previews
overlay-test: tools/hand_overlay_test.c hand_overlay.c hand_overlay.h
	mkdir -p build/tests
	gcc -I. -Ipva/include -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer tools/hand_overlay_test.c hand_overlay.c -lm -o build/tests/hand_overlay_test
	./build/tests/hand_overlay_test
access-test:
	$(MAKE) TARGET=host build/host/vendor/sqlite/sqlite3.o
	mkdir -p build/tests
	gcc -I. -Ivendor/sqlite -Ipva/include -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer tools/access_test.c access.c access_store.c ir_exposure.c ime.c build/host/vendor/sqlite/sqlite3.o -lm -lpthread -o build/tests/access_test
	./build/tests/access_test
exposure-test:
	mkdir -p build/tests
	gcc -I. -Ipva/include -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer tools/ir_exposure_test.c ir_exposure.c -o build/tests/ir_exposure_test
	./build/tests/ir_exposure_test
pva-sync:
	mkdir -p pva/include pva/lib pva/models pva/licenses
	cp $(PVA_SOURCE)/build/cv184x-delivery/include/*.h pva/include/
	cp $(PVA_SOURCE)/build/cv184x-delivery/lib/*.a pva/lib/
	cp $(PVA_SOURCE)/build/cv184x-delivery/models/pva.model $(PVA_SOURCE)/build/cv184x-delivery/models/manifest.json pva/models/
	cp $(PVA_SOURCE)/build/cv184x-delivery/licenses/* pva/licenses/
kernel:
	$(MAKE) -C $(SDK)/linux_5.10 O=$(SDK)/linux_5.10/build/cv1842hp_wevb_0014a_spinand ARCH=arm CROSS_COMPILE=$(TOOL) M=$(CURDIR)/kernel modules
pixel_check: tools/pixel_check.c pixels.c pixels.h
	$(TOOL)gcc -I. -O2 -Wall -Wextra -Werror -mcpu=cortex-a53 -mfpu=neon-vfpv4 -static tools/pixel_check.c pixels.c -o $@
access_inspect: tools/access_inspect.c build/board/vendor/sqlite/sqlite3.o
	$(TOOL)gcc -Ivendor/sqlite -O2 -Wall -Wextra -Werror -static $^ -lm -lpthread -o $@
dvp_probe: tools/dvp_probe.c gc0328.c gc0328.h gc0328_regs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror tools/dvp_probe.c gc0328.c -static -L$(SDK)/cvi_mpi/lib -Wl,--start-group -lvi -lmipi -lsys -Wl,--end-group -lpthread -lm -o $@
dvp_pipeline_check: tools/dvp_pipeline_check.c gc0328.c camera_port.c scale.c pixels.c hand_overlay.c $(SNS_OBJ) gc0328.h gc0328_regs.h media.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror $(filter %.c %.o,$^) -static -L$(SDK)/cvi_mpi/lib -Wl,--start-group -lvi -lmipi -lsys -lisp -lcvi_bin -lisp_algo -lae -lawb -laf -lvpss -lvo -lgdc -lmipi_tx -Wl,--end-group -lpthread -lm -latomic -o $@
.PHONY: package overlay-test pva-sync
package: all kernel pixel_check access_inspect
	mkdir -p build/deploy/licenses build/deploy/pva build/deploy/evidence/pva build/deploy/evidence/access build/deploy/evidence/performance build/deploy/tools
	$(TOOL)strip -o build/deploy/ky50_ui ky50_ui
	cp kernel/ky50_i2c1.ko kernel/ky50_touch.ko kernel/ky50_relay.ko kernel/ky50_amp.ko run.sh stop.sh boot.sh install-local.sh pixel_check tools/soak.sh build/deploy/
	cp README.md THIRD_PARTY.md USER_DATABASE.md access_inspect build/deploy/
	cp tools/make_pinyin.py tools/access_inspect.c build/deploy/tools/
	cp evidence/pva/VALIDATION.md build/deploy/evidence/pva/
	cp evidence/access/VALIDATION.md build/deploy/evidence/access/
	cp evidence/performance/VALIDATION.md evidence/performance/summary.json build/deploy/evidence/performance/
	cp -r assets build/deploy/
	cp pva/models/pva.model pva/models/manifest.json pva/pvaConf.json build/deploy/pva/
	cp pva/licenses/* build/deploy/licenses/
	cp $(PVA_SOURCE)/build/cv184x-delivery/bin/device_authorize build/deploy/
	cp vendor/lvgl/LICENCE.txt build/deploy/licenses/LVGL-MIT.txt
	cp FONT-LICENSE.txt build/deploy/licenses/
	cp licenses/*.txt build/deploy/licenses/
	chmod +x build/deploy/ky50_ui build/deploy/*.sh build/deploy/pixel_check
	cd build/deploy && sha256sum ky50_ui device_authorize access_inspect pixel_check *.ko *.sh assets/*.s16le assets/ime/* pva/* tools/* > SHA256SUMS
clean:
	rm -rf build ky50_ui ui_preview
-include $(OBJ:.o=.d)
