SHELL := /bin/bash

ROOT := $(CURDIR)
PALM_TOOLCHAIN_ROOT ?= ../palm-toolchain
ifeq ($(strip $(PALM_TOOLCHAIN_ROOT)),)
$(error Set PALM_TOOLCHAIN_ROOT to the palm-toolchain repository directory)
endif
TOOLCHAIN_ROOT := $(abspath $(PALM_TOOLCHAIN_ROOT))
PREFIX := $(if $(PALM_TOOLCHAIN_PREFIX),$(PALM_TOOLCHAIN_PREFIX),$(TOOLCHAIN_ROOT)/.toolchain/prefix)
SDK := $(PREFIX)/palmdev/sdk/include
CC := $(PREFIX)/bin/m68k-none-elf-gcc
OBJCOPY := $(PREFIX)/bin/m68k-none-elf-objcopy
SIZE := $(PREFIX)/bin/m68k-none-elf-size
PILRC := $(PREFIX)/bin/pilrc
BUILD_PRC := $(PREFIX)/bin/build-prc

APP_NAME := Map
APP_ID := Map
APP_CREATOR := PMap
APP_TYPE := appl
BUILD_DIR := $(ROOT)/build
ELF := $(BUILD_DIR)/code0001.68k.elf
CODE := $(BUILD_DIR)/code0001.bin
RESOURCES := $(BUILD_DIR)/$(APP_ID).ro
PRC := $(BUILD_DIR)/$(APP_ID).prc
OBJECTS := $(addprefix $(BUILD_DIR)/,main.o startup.o cache.o net.o http.o)
DEPS := $(OBJECTS:.o=.d)

SDK_INCLUDES := -isystem$(SDK) -isystem$(SDK)/Core \
	-isystem$(SDK)/Core/Hardware -isystem$(SDK)/Core/System \
	-isystem$(SDK)/Core/UI -isystem$(SDK)/Dynamic \
	-isystem$(SDK)/Libraries
ABI_FLAGS := -Wno-multichar -m68000 -mno-align-int -mpcrel -fpic \
	-fshort-enums -mshort
CFLAGS := $(ABI_FLAGS) -std=gnu17 -Os -Wall -Wextra -Wno-unused-parameter \
	-Wno-old-style-declaration -MMD -MP -ffunction-sections -fdata-sections \
	-I$(ROOT)/resources -I$(ROOT)/vendor -DPALMOS $(SDK_INCLUDES)
LDFLAGS := $(ABI_FLAGS) -Os -nostartfiles -Wl,--gc-sections \
	-Wl,-T,$(ROOT)/linker.lkr -Wl,-Map,$(BUILD_DIR)/$(APP_ID).map

.PHONY: all test clean

all: $(PRC)

TRUST_ROOTS ?= data/roots.pem

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: src/%.c resources/resource.h | $(BUILD_DIR)
	@test -x "$(CC)" || { echo "Missing Palm compiler; run 'make bootstrap' from $(TOOLCHAIN_ROOT)" >&2; exit 1; }
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJECTS) linker.lkr
	$(CC) -o $@.stage1 $(LDFLAGS) -Wl,--emit-relocs $(OBJECTS)
	node $(TOOLCHAIN_ROOT)/tools/generate-m68k-relocs.mjs \
		$@.stage1 $(BUILD_DIR)/reloc_offsets.s $(PREFIX)/bin/m68k-none-elf-readelf
	$(CC) $(ABI_FLAGS) -c $(BUILD_DIR)/reloc_offsets.s -o $(BUILD_DIR)/reloc_offsets.o
	$(CC) -o $@ $(LDFLAGS) -Wl,--emit-relocs $(OBJECTS) $(BUILD_DIR)/reloc_offsets.o
	rm -f $@.stage1

$(CODE): $(ELF)
	$(OBJCOPY) -O binary $< $@ -j .vec -j .text

$(RESOURCES): $(CODE) $(BUILD_DIR)/maparm.bin resources/app.rcp resources/resource.h $(TRUST_ROOTS) $(wildcard data/*)
	cp "$(TRUST_ROOTS)" "$(BUILD_DIR)/roots.pem"
	cd $(BUILD_DIR) && $(PILRC) -ro -o $@ -creator $(APP_CREATOR) \
		-type $(APP_TYPE) -name '$(APP_NAME)' -I $(ROOT)/resources -I $(ROOT)/data \
		$(ROOT)/resources/app.rcp

$(PRC): $(RESOURCES)
	$(BUILD_PRC) --no-check -o $@ -n '$(APP_NAME)' -c $(APP_CREATOR) \
		-t $(APP_TYPE) $(RESOURCES)
	$(SIZE) $(ELF)
	@echo "Built $@"

test: $(PRC) test-native
	node $(TOOLCHAIN_ROOT)/tools/validate-prc.mjs \
		$(PRC) '$(APP_NAME)' $(APP_TYPE) $(APP_CREATOR)

	python3 tools/check-prc.py $(PRC) $(BUILD_DIR)/maparm.bin

.PHONY: test-native
test-native: $(BUILD_DIR)/native-test
	"$(BUILD_DIR)/native-test" data/world3-18.png

$(BUILD_DIR)/native-test: src/native.c tests/native_test.c | $(BUILD_DIR)
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -DMAP_HOST_TEST $^ -lz -o $@

HOST_CC ?= cc

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)

$(BUILD_DIR)/http.o: vendor/http.c vendor/http.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

ARM := $(PREFIX)/bin/arm-none-eabi-
ARM_ABI := -march=armv4t -marm -mfloat-abi=soft
ARM_FLAGS := $(ARM_ABI) -Os -std=gnu17 -Wall -Wextra -ffreestanding -fno-builtin \
	-fpie -ffunction-sections -fdata-sections -MMD -MP -DZ_SOLO -DNO_GZIP \
	-Ivendor/zlib -fno-unwind-tables -fno-asynchronous-unwind-tables
ARM_OBJECTS := $(addprefix $(BUILD_DIR)/arm-,native.o adler32.o inffast.o inflate.o inftrees.o zutil.o)

$(BUILD_DIR)/arm-native.o: src/native.c | $(BUILD_DIR)
	$(ARM)gcc $(ARM_FLAGS) -c $< -o $@

$(BUILD_DIR)/arm-%.o: vendor/zlib/%.c | $(BUILD_DIR)
	$(ARM)gcc $(ARM_FLAGS) -c $< -o $@

$(BUILD_DIR)/maparm.elf: $(ARM_OBJECTS) src/native.ld
	$(ARM)gcc $(ARM_ABI) -nostdlib -Wl,--gc-sections,-T,src/native.ld -o $@ $(ARM_OBJECTS) -lgcc

$(BUILD_DIR)/maparm.bin: $(BUILD_DIR)/maparm.elf
	$(ARM)objcopy -O binary -j .text $< $@

-include $(ARM_OBJECTS:.o=.d)
