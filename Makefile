# Makefile -- GTA: San Andreas (Android 2.11.311 arm64) for Linux handhelds
#
#   make                       native build (aarch64 target system or inside its SDK)
#   make CROSS=aarch64-linux-gnu- PKG_CONFIG=...   cross build
#   make test                  test suite on a PC (tests/run-tests.sh: ABI, loader
#                              and shims under qemu, platform layer under weston)
#
# Libraries: SDL2 (input only: game controllers), wayland-client,
# wayland-egl, EGL, GLESv2, OpenAL, libmpg123, zlib; wayland-scanner and
# wayland-protocols at build time. pkg-config is used when PKG_CONFIG works,
# otherwise set PKG_CFLAGS/PKG_LIBS (and WAYLAND_PROTOCOLS).

CROSS     ?=
# a CC from the environment (LibreELEC's toolchain) or the command line wins
ifeq ($(origin CC),default)
CC        := $(CROSS)gcc
endif
PKG_CONFIG ?= pkg-config
VERSION   ?= $(shell git describe --always --dirty 2>/dev/null || echo dev)

UPSTREAM  := upstream/source
SRC       := src

PKGS      := sdl2 wayland-client wayland-egl egl glesv2 openal libmpg123 zlib
PKG_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   ?= $(shell $(PKG_CONFIG) --libs $(PKGS) 2>/dev/null || echo -lSDL2 -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lopenal -lmpg123 -lz)

# xdg-shell and viewporter client code, generated from wayland-protocols
WAYLAND_SCANNER   ?= wayland-scanner
WAYLAND_PROTOCOLS ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols 2>/dev/null)
XDG_SHELL_XML     := $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
VIEWPORTER_XML    := $(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml
PROTO_H   := build/xdg-shell-client-protocol.h build/viewporter-client-protocol.h

TUNE      ?= -march=armv8-a+crc -mtune=cortex-a35
OPT       ?= -O2
CFLAGS    ?=
CFLAGS    += -std=gnu11 $(OPT) -g -fPIE -fno-omit-frame-pointer -fno-strict-aliasing \
             -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
             -I$(SRC) -I$(SRC)/compat -I$(UPSTREAM) -Ibuild $(PKG_CFLAGS) \
             -DGTASA_VERSION='"$(VERSION)"'
ASFLAGS   += -DSILENT_MP3_PATH='"upstream/data/silent.bin"'
LDFLAGS   += -pie -Wl,-z,noexecstack
LIBS      := $(PKG_LIBS) -Wl,--no-as-needed -lz -lm -Wl,--as-needed -lpthread -ldl

ifneq ($(filter aarch64%,$(shell $(CC) -dumpmachine)),)
CFLAGS    += $(TUNE)
endif

# tls_guard.o first: its TLS array has to open the executable's TLS block
# (TPIDR_EL0+16); see tls_guard.c
OURS := tls_guard loader bionic_libc bionic_pthread bionic_signal android import_table \
        gl_wrap egl_wrap platform platform_config gamenative stats crash build_check zipx import \
        casefold game_tuning util error main
OBJS := $(OURS:%=build/%.o) build/bionic_setjmp.o build/silent_mp3.o \
        build/xdg-shell-protocol.o build/viewporter-protocol.o \
        build/up_config.o build/up_jni_fake.o build/up_overlay.o build/up_game.o build/up_openal.o \
        $(patsubst $(UPSTREAM)/hooks/%.s,build/up_%.o,$(wildcard $(UPSTREAM)/hooks/*.s))

all: gtasa

gtasa: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

build/%.o: $(SRC)/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/platform.o: $(PROTO_H)

build/xdg-shell-client-protocol.h: $(XDG_SHELL_XML) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/xdg-shell-protocol.c: $(XDG_SHELL_XML) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/viewporter-client-protocol.h: $(VIEWPORTER_XML) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/viewporter-protocol.c: $(VIEWPORTER_XML) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/%-protocol.o: build/%-protocol.c
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: $(SRC)/%.S | build
	$(CC) $(CFLAGS) $(ASFLAGS) -c -o $@ $<

build/up_%.o: $(UPSTREAM)/%.c | build
	$(CC) $(CFLAGS) -Wno-unused-variable -Wno-unused-function -c -o $@ $<

build/up_%.o: $(UPSTREAM)/hooks/%.c | build
	$(CC) $(CFLAGS) -Wno-unused-variable -Wno-unused-function -c -o $@ $<

build/up_%.o: $(UPSTREAM)/hooks/%.s | build
	$(CC) -c -o $@ $<

build:
	mkdir -p build

test:
	tests/run-tests.sh

clean:
	rm -rf build gtasa tests/build

.PHONY: all clean test
