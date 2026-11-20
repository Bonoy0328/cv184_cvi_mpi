SHELL := /bin/bash
SDK := $(abspath ../../..)
TOOL := $(SDK)/host-tools/gcc/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf/bin/arm-none-linux-musleabihf-
# Product feature switches. Set each to 0 (off) or 1 (on).
KY50_ENABLE_FINGERPRINT ?= 0
KY50_ENABLE_FACE ?= 0
KY50_ENABLE_PALM ?= 0
FEATURE_SWITCHES := KY50_ENABLE_FINGERPRINT KY50_ENABLE_FACE KY50_ENABLE_PALM
$(foreach f,$(FEATURE_SWITCHES),$(if $(or $(filter-out 0 1,$($(f))),$(filter-out 1,$(words $($(f))))),$(error $(f) must be 0 or 1)))
ENABLED := $(filter 1,$(foreach f,$(FEATURE_SWITCHES),$($(f))))
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
APP := ui.c status.c fonts.c input.c main.c palm_queue.c camera_port.c display_port.c scale.c pixels.c settings.c factory.c rtsp_stream.c gc0328.c hand_overlay.c user_ui.c ime.c access.c access_store.c
PVA_SOURCE := $(SDK)/libpva_face
ifneq ($(ENABLED),)
CPP_APP := handpose_bridge.cpp
PVA_LIBS := pva/lib/libpva.a pva/lib/libdeviceCerf.a pva/lib/libsodium.a
BM_LIB := $(SDK)/install/soc_cv1842hp_wevb_0014a_spinand/rootfs/system/lib
CPPFLAGS += -Ipva/include -I$(PVA_SOURCE)/third_party/nlohmann
PVA_LINK := -lpva -ldeviceCerf -lsodium
PVA_RUNTIME := -L$(BM_LIB) -lbmrt -lbmlib -lstdc++fs -Wl,-rpath,/system/lib -Wl,-rpath-link,$(BM_LIB)
CPPFLAGS += -I$(PVA_SOURCE)/src/api -I$(PVA_SOURCE)/src/core
endif
CPPFLAGS += -I$(SDK)/cvi_mpi/include -I$(SDK)/cvi_mpi/include/isp -I$(SNS)/sensor_i2c
CPPFLAGS += -I$(SDK)/osal/include
CPPFLAGS += -I$(SDK)/cvi_mpi/self_test/audio_ut/audio_pcm -I$(SDK)/linux_5.10/sound/soc/cvitek
RTSP_DIR := $(SDK)/ipcamera/components/comps_install/arm-none-linux-musleabihf
RTSP_LIBS := $(RTSP_DIR)/rtsp/lib/libcomp_rtsp.a $(RTSP_DIR)/ringbuffer/lib/libcomp_ringbuffer.a $(RTSP_DIR)/osal/lib/libcomp_osal.a
CPPFLAGS += -I$(RTSP_DIR)/rtsp/include
CPPFLAGS += -DARCH_CV184X
CPPFLAGS += -I$(SDK)/linux_5.10/build/cv1842hp_wevb_0014a_spinand/arm/usr/include
LIBS := -static-libstdc++ -static-libgcc -Wl,-Bstatic -L$(SDK)/cvi_mpi/lib -Lpva/lib -Wl,--start-group -lvi -lmipi -lmipi_tx -lsys -lisp -lcvi_bin -lawb -lae -laf -lisp_algo -lvpss -lvo -lgdc -ltinyalsa $(PVA_LINK) -lvenc $(RTSP_LIBS) -Wl,--end-group -Wl,-Bdynamic \
    $(PVA_RUNTIME) -lpthread -lm -latomic -ldl
endif
APP += ir_exposure.c app_config.c biometric_worker.c relay_control.c face_ir_calibration.c
CPPFLAGS += -I. -Ivendor/lvgl -Ivendor/lvgl/include -DLV_CONF_INCLUDE_SIMPLE -Ivendor/sqlite -Ipva/include
CFLAGS := -std=gnu11 -O2 -g -ffunction-sections -fdata-sections -Wall -Wextra $(ARCH_FLAGS)
LV_SRC := $(sort $(shell rg --files vendor/lvgl/src -g '*.c'))
OBJ := $(addprefix build/$(TARGET)/,$(APP:.c=.o) $(CPP_APP:.cpp=.o) $(LV_SRC:.c=.o)) build/$(TARGET)/vendor/sqlite/sqlite3.o
SNS_OBJ := $(addprefix build/$(TARGET)/sensor/,$(notdir $(SNS_SRC:.c=.o)))
.PHONY: all host test kernel clean
all: $(BIN)
# Only the feature-aware objects need recompiling when the selection changes.
FEATURE_HEADER := build/$(TARGET)/ky50_features.h
FEATURE_OBJECTS := main ui user_ui preview handpose_bridge
$(addprefix build/$(TARGET)/,$(addsuffix .o,$(FEATURE_OBJECTS))): $(FEATURE_HEADER)
CPPFLAGS += -Ibuild/$(TARGET)
.PHONY: force-features
$(FEATURE_HEADER): force-features
	@mkdir -p $(@D)
	@{ $(foreach f,$(FEATURE_SWITCHES),echo '#define $(f) $($(f))';) \
	   echo '#define KY50_BIOMETRICS (KY50_ENABLE_PALM || KY50_ENABLE_FACE || KY50_ENABLE_FINGERPRINT)'; } > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(BIN): $(OBJ) $(SNS_OBJ) $(PVA_LIBS) $(RTSP_LIBS)
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
test: host overlay-test access-test exposure-test queue-test biometric-test async-test store-wal-test relay-test fingerprint-test
	mkdir -p build/previews
	./ui_preview build/previews
overlay-test: tools/hand_overlay_test.c hand_overlay.c hand_overlay.h
	mkdir -p build/tests
	gcc -I. -Ipva/include -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer tools/hand_overlay_test.c hand_overlay.c -lm -o build/tests/hand_overlay_test
	./build/tests/hand_overlay_test
access-test:
	$(MAKE) TARGET=host build/host/vendor/sqlite/sqlite3.o
	mkdir -p build/tests
	gcc -I. -Ivendor/sqlite -Ipva/include -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer tools/access_test.c access.c access_store.c biometric_worker.c ir_exposure.c app_config.c face_ir_calibration.c ime.c build/host/vendor/sqlite/sqlite3.o -lm -lpthread -o build/tests/access_test
	./build/tests/access_test
exposure-test:
	mkdir -p build/tests
	gcc -I. -Ipva/include -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer tools/ir_exposure_test.c ir_exposure.c -o build/tests/ir_exposure_test
	./build/tests/ir_exposure_test
pva-sync:
	mkdir -p pva/include pva/lib pva/models pva/licenses
	cp $(PVA_SOURCE)/build/cv184x-delivery/include/*.h pva/include/
	cp $(PVA_SOURCE)/build/cv184x-delivery/lib/*.a pva/lib/
	cp $(PVA_SOURCE)/build/cv184x-delivery/models/pva.model $(PVA_SOURCE)/build/cv184x-delivery/models/manifest.json $(PVA_SOURCE)/build/cv184x-delivery/models/pva_face.model $(PVA_SOURCE)/build/cv184x-delivery/models/face* $(PVA_SOURCE)/build/cv184x-delivery/models/pva_fingerprint.model* $(PVA_SOURCE)/build/cv184x-delivery/models/fingerprint* pva/models/
	cp $(PVA_SOURCE)/build/cv184x-delivery/licenses/* pva/licenses/
	cd pva && sha256sum include/*.h lib/*.a models/* pvaConf.json licenses/* > SHA256SUMS
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
# Minimal payload shared by the board image and the standalone deployment.
RUNTIME := build/runtime
.PHONY: runtime firmware-install
runtime: all kernel
	mkdir -p $(RUNTIME)/assets/ime $(RUNTIME)/licenses
	$(TOOL)strip -o $(RUNTIME)/ky50_ui ky50_ui
	install -m 755 run.sh stop.sh $(RUNTIME)/
	install -m 644 kernel/ky50_{i2c1,touch,relay,lights,amp}.ko $(RUNTIME)/
	install -m 644 assets/ime/pinyin.tsv $(RUNTIME)/assets/ime/
	install -m 644 assets/recognition_success.s16le $(RUNTIME)/assets/
	install -m 644 vendor/lvgl/LICENCE.txt $(RUNTIME)/licenses/LVGL-MIT.txt
	install -m 644 FONT-LICENSE.txt licenses/*.txt $(RUNTIME)/licenses/
	rm -rf $(RUNTIME)/pva $(RUNTIME)/assets/face_ir_calibration.conf
ifneq ($(ENABLED),)
	mkdir -p $(RUNTIME)/pva
	python3 tools/package_pva.py $(RUNTIME)/pva $(KY50_ENABLE_PALM) $(KY50_ENABLE_FACE) $(KY50_ENABLE_FINGERPRINT)
	install -m 644 pva/licenses/* $(RUNTIME)/licenses/
endif
ifeq ($(KY50_ENABLE_FACE),1)
	install -m 644 assets/face_ir_calibration.conf $(RUNTIME)/assets/
endif
firmware-install: runtime
	test -n "$(ROOTFS_DIR)" && test -d "$(ROOTFS_DIR)/etc/init.d"
	mkdir -p "$(ROOTFS_DIR)/system/ky50-ui"
	rm -rf "$(ROOTFS_DIR)/system/ky50-ui/pva" "$(ROOTFS_DIR)/system/ky50-ui/assets/face_ir_calibration.conf"
	cp -a $(RUNTIME)/. "$(ROOTFS_DIR)/system/ky50-ui/"
	install -m 755 firmware-init.sh "$(ROOTFS_DIR)/etc/init.d/S99zz_ky50_ui"
.PHONY: package overlay-test pva-sync
package: runtime pixel_check access_inspect
	mkdir -p build/deploy/licenses build/deploy/evidence/pva build/deploy/evidence/access build/deploy/evidence/performance build/deploy/evidence/lights build/deploy/tools
	rm -rf build/deploy/pva build/deploy/assets/face_ir_calibration.conf build/deploy/device_authorize
	cp -a $(RUNTIME)/. build/deploy/
	cp kernel/ky50_i2c1.ko kernel/ky50_touch.ko kernel/ky50_relay.ko kernel/ky50_lights.ko kernel/ky50_amp.ko run.sh stop.sh boot.sh install-local.sh pixel_check tools/soak.sh build/deploy/
	cp README.md DEVELOPMENT.md THIRD_PARTY.md USER_DATABASE.md access_inspect build/deploy/
	cp tools/make_pinyin.py tools/access_inspect.c build/deploy/tools/
	cp tools/check_rtsp.py tools/rtsp_check.c build/deploy/tools/
	mkdir -p build/deploy/evidence/rtsp
	cp evidence/rtsp/* build/deploy/evidence/rtsp/
	cp evidence/pva/VALIDATION.md build/deploy/evidence/pva/
	cp evidence/access/VALIDATION.md build/deploy/evidence/access/
	cp evidence/performance/VALIDATION.md evidence/performance/summary.json build/deploy/evidence/performance/
	cp -r evidence/lights/. build/deploy/evidence/lights/
	mkdir -p build/deploy/evidence/fingerprint
	cp -r evidence/fingerprint/. build/deploy/evidence/fingerprint/
	mkdir -p build/deploy/evidence/async-recognition
	cp evidence/async-recognition/* build/deploy/evidence/async-recognition/
	mkdir -p build/deploy/evidence/success-relay
	cp evidence/success-relay/* build/deploy/evidence/success-relay/
	mkdir -p build/deploy/evidence/palm-ae
	cp evidence/palm-ae/* build/deploy/evidence/palm-ae/
	mkdir -p build/deploy/evidence/ir-strong
	cp evidence/ir-strong/* build/deploy/evidence/ir-strong/
	mkdir -p build/deploy/evidence/image-resample
	cp evidence/image-resample/* build/deploy/evidence/image-resample/
	cp -r assets/ime build/deploy/assets/
ifneq ($(ENABLED),)
	cp $(PVA_SOURCE)/build/cv184x-delivery/bin/device_authorize build/deploy/
endif
	cp vendor/lvgl/LICENCE.txt build/deploy/licenses/LVGL-MIT.txt
	cp FONT-LICENSE.txt build/deploy/licenses/
	cp licenses/*.txt build/deploy/licenses/
	chmod +x build/deploy/ky50_ui build/deploy/*.sh build/deploy/pixel_check
	cd build/deploy && sha256sum ky50_ui $(if $(ENABLED),device_authorize) access_inspect pixel_check *.ko *.sh assets/*.s16le $(if $(filter 1,$(KY50_ENABLE_FACE)),assets/face_ir_calibration.conf) assets/ime/* $(if $(ENABLED),pva/*) tools/* > SHA256SUMS
clean:
	rm -rf build ky50_ui ui_preview
-include $(OBJ:.o=.d)

.PHONY: queue-test biometric-test
.PHONY: relay-test
relay-test:
	mkdir -p build/tests
	gcc -I. -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined tools/relay_control_test.c -o build/tests/relay_control_test
	./build/tests/relay_control_test
queue-test:
	mkdir -p build/tests
	gcc -I. -Ipva/include -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer tools/palm_queue_test.c palm_queue.c -lpthread -o build/tests/palm_queue_test
	./build/tests/palm_queue_test
biometric-test:
	$(MAKE) TARGET=host build/host/vendor/sqlite/sqlite3.o
	mkdir -p build/tests
	gcc -I. -Ipva/include -Ivendor/sqlite -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer tools/biometric_test.c access.c access_store.c biometric_worker.c ir_exposure.c app_config.c face_ir_calibration.c build/host/vendor/sqlite/sqlite3.o -lm -lpthread -o build/tests/biometric_test
	./build/tests/biometric_test

.PHONY: calibration-test
.PHONY: async-test store-wal-test
async-test:
	$(MAKE) TARGET=host build/host/vendor/sqlite/sqlite3.o
	mkdir -p build/tests
	gcc -I. -Ipva/include -Ivendor/sqlite -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer tools/async_biometric_test.c access.c access_store.c biometric_worker.c ir_exposure.c app_config.c face_ir_calibration.c build/host/vendor/sqlite/sqlite3.o -lm -lpthread -o build/tests/async_biometric_test
	./build/tests/async_biometric_test
store-wal-test:
	$(MAKE) TARGET=host build/host/vendor/sqlite/sqlite3.o
	mkdir -p build/tests
	gcc -I. -Ivendor/sqlite -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer tools/store_wal_test.c access_store.c build/host/vendor/sqlite/sqlite3.o -lm -lpthread -o build/tests/store_wal_test
	./build/tests/store_wal_test
calibration-test:
	mkdir -p build/tests
	gcc -I. -Ipva/include -Ivendor/sqlite -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer tools/face_ir_calibration_test.c face_ir_calibration.c biometric_worker.c -lm -lpthread -o build/tests/face_ir_calibration_test
	./build/tests/face_ir_calibration_test

rtsp_check: tools/rtsp_check.c build/board/camera_port.o build/board/gc0328.o build/board/rtsp_stream.o $(SNS_OBJ) $(RTSP_LIBS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror -c tools/rtsp_check.c -o build/board/rtsp_check.o
	$(LINK) build/board/rtsp_check.o build/board/camera_port.o build/board/gc0328.o build/board/rtsp_stream.o $(SNS_OBJ) $(LIBS) -o $@

.PHONY: fingerprint-test
fingerprint-test:
	$(MAKE) TARGET=host build/host/vendor/sqlite/sqlite3.o
	mkdir -p build/tests
	gcc -I. -Ipva/include -Ivendor/sqlite -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer tools/fingerprint_test.c access.c access_store.c biometric_worker.c ir_exposure.c app_config.c face_ir_calibration.c build/host/vendor/sqlite/sqlite3.o -lm -lpthread -o build/tests/fingerprint_test
	./build/tests/fingerprint_test
