# apfs-tool - GNU make build
#
#   make                 build apfs-tool and dmg into build/
#   make test            run the test suite
#   make test CLEAN_TEST_IMAGES=1   run tests, then remove generated images
#   make clean-test-images          remove generated test images only
#   make install         install into $(PREFIX)/bin
#   make CC=x86_64-w64-mingw32-gcc   cross-compile for Windows
#
# Requirements: a C99 compiler, zlib and json-c (POSIX manifest commands).

CC ?= cc
AR ?= ar
PREFIX ?= /usr/local
BUILD ?= build
CLEAN_TEST_IMAGES ?= 0

UNAME_S := $(shell uname -s 2>/dev/null)
TARGET_WIN := $(if $(findstring mingw,$(CC)),1,$(if $(findstring MINGW,$(UNAME_S)),1,))

CFLAGS ?= -O2 -g
override CFLAGS += -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-Iincludes -Ithird_party/lzfse \
	-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_POSIX_C_SOURCE=200809L \
	-D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
LDLIBS += -lz

EXE :=
ifeq ($(TARGET_WIN),1)
EXE := .exe
override CFLAGS += -D__USE_MINGW_ANSI_STDIO=1
else
override CFLAGS += $(shell pkg-config --cflags json-c 2>/dev/null)
LDLIBS += $(or $(shell pkg-config --libs json-c 2>/dev/null),-ljson-c)
endif

LZFSE_SRC := third_party/lzfse/lzfse_decode.c third_party/lzfse/lzfse_decode_base.c \
	third_party/lzfse/lzfse_encode.c third_party/lzfse/lzfse_encode_base.c \
	third_party/lzfse/lzfse_fse.c third_party/lzfse/lzvn_decode_base.c \
	third_party/lzfse/lzvn_encode_base.c

COMMON_SRC := common/abstractfile.c

APFS_SRC := apfs/util.c apfs/unicode.c apfs/btree.c apfs/omap.c apfs/spaceman.c \
	apfs/container.c apfs/volume.c apfs/fs.c apfs/extents.c apfs/rawfile.c \
	apfs/image.c apfs/debug.c apfs/snapshot.c apfs/compress.c apfs/file.c \
	apfs/mkfs.c apfs/platform.c apfs/apfslib.c apfs/tar.c apfs/udif.c \
	apfs/resize.c apfs/seal.c apfs/verify.c apfs/trustcache.c apfs/mtree.c \
	apfs/crypto.c apfs/encrypted.c apfs/manifest.c

LIB_SRC := $(COMMON_SRC) $(APFS_SRC) $(LZFSE_SRC)
LIB_OBJ := $(LIB_SRC:%.c=$(BUILD)/obj/%.o)
LIB := $(BUILD)/libapfs.a

TOOL := $(BUILD)/apfs-tool$(EXE)
DMG := $(BUILD)/dmg$(EXE)
UNIT := $(BUILD)/unit$(EXE)

.PHONY: all clean clean-test-images install test

all: $(TOOL) $(DMG)

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# lzfse is third-party code: build it without our warning set
$(BUILD)/obj/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -Wall -Wextra,$(CFLAGS)) -w -c $< -o $@

$(LIB): $(LIB_OBJ)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(TOOL): $(BUILD)/obj/apfs/apfs-tool.o $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(DMG): $(BUILD)/obj/apfs/dmg-tool.o $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(UNIT): $(BUILD)/obj/tests/unit.o $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# APFS_TEST_FSCK=0 skips the fsck_apfs cross-checks on macOS
test: $(TOOL) $(DMG) $(UNIT)
	@mkdir -p $(BUILD)/test
	$(UNIT) $(BUILD)/test
	sh tests/run.sh $(TOOL) $(BUILD)/test
ifeq ($(CLEAN_TEST_IMAGES),1)
	$(MAKE) --no-print-directory clean-test-images BUILD="$(BUILD)"
endif

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TOOL) $(DMG) $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -rf $(BUILD)

# Keep binaries, object files, logs and small metadata while removing disk
# images left by tests. Scanning all of BUILD also covers custom test output
# directories made by direct tests/run.sh invocations.
clean-test-images:
	@if [ -d "$(BUILD)" ]; then \
		find "$(BUILD)" \( -type f -o -type l \) \( \
			-name '*.raw' -o -name '*.img' -o -name '*.dmg' -o \
			-name '*.im4p' -o -name '*.aea' \
		\) -delete; \
	fi

-include $(LIB_OBJ:.o=.d) $(BUILD)/obj/apfs/apfs-tool.d $(BUILD)/obj/apfs/dmg-tool.d $(BUILD)/obj/tests/unit.d
