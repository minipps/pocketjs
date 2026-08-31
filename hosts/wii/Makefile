# PocketJS Nintendo Wii sample DOL.
#
# The target-specific build tool (tools/wii.ts) supplies every artifact path
# and the resolved manifest values through the environment.  This Makefile is
# deliberately small: devkitPPC/libogc own the Wii startup/link rules, while
# the shared core and QuickJS archives are built before make is invoked. The
# Wii host archive is emitted alongside them so an embedding app can reuse the
# public runtime without linking this sample's main loop.

DEVKITPRO ?= /opt/devkitpro
DEVKITPPC ?= $(DEVKITPRO)/devkitPPC

WII_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
include $(DEVKITPPC)/wii_rules

HOST := $(dir $(WII_MAKEFILE))
SOURCE := $(HOST)/src
SHARED := $(HOST)/../shared
BUILD ?= $(HOST)/build
OUT ?= $(HOST)/pocketjs-wii.dol

LIBOGC_INC ?= $(DEVKITPRO)/libogc/include
LIBOGC_LIB ?= $(DEVKITPRO)/libogc/lib/wii
OGC_SPECS ?= $(DEVKITPRO)/libogc/share/ogc.specs
ELF2DOL ?= $(DEVKITPRO)/tools/bin/elf2dol

POCKETJS_QUICKJS_DIR ?= $(BUILD)/quickjs
POCKETJS_QUICKJS_LIB ?= $(POCKETJS_QUICKJS_DIR)/libquickjs.a
POCKETJS_CORE_LIB ?= $(BUILD)/libpocketjs_core_cabi.a
POCKETJS_WII_LIB ?= $(BUILD)/libpocketjs_wii.a
POCKETJS_APP_POCKET ?= $(BUILD)/app.pocket

# wii_rules' MACHDEP is the SDK's source of truth.  Keep the expanded flags
# visible here too: they are the ABI this host is reviewed against.
WII_ARCH := -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float
CFLAGS := -g -O2 -Wall -Wextra -std=gnu11 $(WII_ARCH) \
  -ffunction-sections -fdata-sections -DJS_NO_NAN_BOXING \
  -DPOCKETJS_TARGET_ID='"$(POCKETJS_TARGET)"' \
  -DPOCKETJS_HOST_ABI=$(POCKETJS_HOST_ABI) \
  -DPOCKETJS_VIEW_W=$(POCKETJS_LOGICAL_WIDTH) \
  -DPOCKETJS_VIEW_H=$(POCKETJS_LOGICAL_HEIGHT) \
  -DPOCKETJS_INTEGER_FIT=$(if $(filter integer-fit,$(POCKETJS_PRESENTATION)),1,0) \
  -DPOCKETJS_RASTER_DENSITY=$(POCKETJS_RASTER_DENSITY) \
  -I$(SHARED) -I$(HOST)/include -I$(BUILD) -I$(POCKETJS_QUICKJS_DIR) \
  -I$(LIBOGC_INC)

# `-specs=ogc.specs` is the SDK spelling; use the installed absolute path so
# GCC does not depend on the caller's working directory or GCC search path.
LDFLAGS := -g $(WII_ARCH) -specs=$(OGC_SPECS) -Wl,--gc-sections \
  -Wl,-Map,$(BUILD)/pocketjs-wii.map
LIBPATHS := -L$(LIBOGC_LIB) -L$(dir $(POCKETJS_CORE_LIB)) \
  -L$(dir $(POCKETJS_QUICKJS_LIB))
# Keep libogc after the guest archives: the Wii archive exports the runtime
# services used by both the sample entry point and the HostOps bridge.
LIBS := -lwiiuse -lbte -logc -lm

POCKET_BIN := $(BUILD)/app_pocket.bin
POCKET_OBJECT := $(BUILD)/app_pocket.bin.o
POCKET_HEADER := $(BUILD)/app_pocket_bin.h
RUNTIME_SOURCES := $(wildcard $(SOURCE)/pocket_wii.c $(SOURCE)/pocket_wii_gx.c)
RUNTIME_OBJECTS := $(patsubst $(SOURCE)/%.c,$(BUILD)/%.o,$(RUNTIME_SOURCES))
WII_LIBRARY_OBJECTS := $(BUILD)/pocket_wii_qjs.o $(RUNTIME_OBJECTS)
OBJECTS := $(BUILD)/main.o $(BUILD)/input.o $(POCKET_OBJECT)
ELF := $(BUILD)/pocketjs-wii.elf

.PHONY: all clean
all: $(OUT) $(POCKETJS_WII_LIB)

$(BUILD):
	@mkdir -p $@

# Copy the target-thinned package into the build directory before bin2o runs.
# Keeping a local name gives bin2s a stable app_pocket_bin symbol and avoids
# embedding a host-specific absolute path in generated assembler.
$(POCKET_BIN): $(POCKETJS_APP_POCKET) | $(BUILD)
	@cp "$<" "$@.new"
	@cmp -s "$@.new" "$@" || mv "$@.new" "$@"
	@rm -f "$@.new"

# bin2o is a make macro supplied by devkitPPC/base_tools.  It emits both the
# app_pocket_bin.h declaration and the aligned app.pocket.o object.
WII_BIN2O_INPUT ?= $(POCKET_BIN)
.PHONY: wii-bin2o
wii-bin2o: $(WII_BIN2O_INPUT)
	$(bin2o)

$(POCKET_OBJECT) $(POCKET_HEADER) &: $(POCKET_BIN)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(WII_MAKEFILE) \
	  wii-bin2o WII_BIN2O_INPUT=$(notdir $(POCKET_BIN))

$(BUILD)/%.o: $(SOURCE)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/main.o: $(POCKET_HEADER)

$(BUILD)/pocket_wii_qjs.o: $(SOURCE)/pocket_wii_qjs.c \
  $(SHARED)/qjs.c $(SHARED)/qjs.h $(SHARED)/pocket_core.h \
  $(POCKETJS_QUICKJS_DIR)/quickjs.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Keep all guest-side objects in one distributable archive. ADDLIB expands the
# two dependency archives into members rather than nesting archives (nested
# archives are opaque to the linker). The group at link time handles Rust's
# cross-member references after the archive has been flattened.
$(POCKETJS_WII_LIB): $(WII_LIBRARY_OBJECTS) $(POCKETJS_CORE_LIB) \
  $(POCKETJS_QUICKJS_LIB) | $(BUILD)
	@mkdir -p "$(dir $@)"
	@{ \
		echo "CREATE $@"; \
		for object in $(WII_LIBRARY_OBJECTS); do echo "ADDMOD $$object"; done; \
		echo "ADDLIB $(POCKETJS_CORE_LIB)"; \
		echo "ADDLIB $(POCKETJS_QUICKJS_LIB)"; \
		echo "SAVE"; \
		echo "END"; \
	} | $(AR) -M
	@$(RANLIB) "$@"

$(ELF): $(OBJECTS) $(POCKETJS_WII_LIB)
	$(CC) $(LDFLAGS) $(OBJECTS) -Wl,--start-group $(POCKETJS_WII_LIB) \
	  -Wl,--end-group $(LIBPATHS) $(LIBS) -o $@

$(OUT): $(ELF)
	@mkdir -p $(dir $@)
	$(ELF2DOL) $< $@

clean:
	@rm -rf $(BUILD)
	@rm -f "$(POCKETJS_WII_LIB)"
