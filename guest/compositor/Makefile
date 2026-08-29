# RemotePipe Wayland compositor for a real Linux host.

PKGS          := wayland-server libcjson xkbcommon xcb xcb-composite
MEDIA_PKGS    := libavcodec libavutil libswscale
CLIENT_PKGS   := wayland-client
CFLAGS        += -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
	$(shell pkg-config --cflags $(PKGS) $(MEDIA_PKGS) 2>/dev/null)
LDLIBS        += $(shell pkg-config --libs $(PKGS) $(MEDIA_PKGS) 2>/dev/null) \
	-lm -pthread

PROTOCOL_DIR   := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML  := $(PROTOCOL_DIR)/stable/xdg-shell/xdg-shell.xml
VIEWPORTER_XML := $(PROTOCOL_DIR)/stable/viewporter/viewporter.xml
FRACTIONAL_XML := $(PROTOCOL_DIR)/staging/fractional-scale/fractional-scale-v1.xml
DECORATION_XML := $(PROTOCOL_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
TEXT_INPUT_XML := $(PROTOCOL_DIR)/unstable/text-input/text-input-unstable-v3.xml
DMABUF_XML     := $(PROTOCOL_DIR)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
XWAYLAND_XML   := $(PROTOCOL_DIR)/staging/xwayland-shell/xwayland-shell-v1.xml
FIFO_XML       := fifo-v1.xml

GENERATED := xdg-shell-server-protocol.h xdg-shell-client-protocol.h xdg-shell-protocol.c \
	viewporter-server-protocol.h viewporter-protocol.c \
	fractional-scale-v1-server-protocol.h fractional-scale-v1-protocol.c \
	xdg-decoration-server-protocol.h xdg-decoration-protocol.c \
	text-input-v3-server-protocol.h text-input-v3-client-protocol.h \
	text-input-v3-protocol.c \
	linux-dmabuf-unstable-v1-server-protocol.h linux-dmabuf-unstable-v1-protocol.c \
	xwayland-shell-v1-server-protocol.h xwayland-shell-v1-protocol.c \
	fifo-v1-server-protocol.h fifo-v1-protocol.c

SOURCES := compositor.c surface.c surface_commit.c presentation.c \
	subcompositor.c input.c host_session.c decoration.c scale.c scene.c region.c \
	perf.c hostlink.c windowwire.c window_events.c xdg_shell.c text_input.c fifo.c \
	data_device.c cursor_shape.c medialink.c dmabuf_remote.c syncobj_stub.c \
	xwayland.c remotepipe_main.c ../encoder/encoder.c ../encoder/alpha.c \
	cursor-shape-v1-protocol.c xdg-shell-protocol.c viewporter-protocol.c \
	fractional-scale-v1-protocol.c xdg-decoration-protocol.c \
	text-input-v3-protocol.c linux-dmabuf-unstable-v1-protocol.c fifo-v1-protocol.c

SOURCES += xwayland-shell-v1-protocol.c

ARCH        := $(shell uname -m | sed 's/^arm64$$/aarch64/')
LIBC        := $(shell ldd --version 2>&1 | grep -qi musl && echo musl || echo gnu)
BUILD_DIR   := .build/$(ARCH)-$(LIBC)
BINARY      := $(BUILD_DIR)/remotepipe-wayland
SHMCLIENT   := $(BUILD_DIR)/shmclient
VKPRESENT   := $(BUILD_DIR)/vkpresent
DIST_BINARY := dist/remotepipe-wayland-$(ARCH)-$(LIBC)
SOURCE_HASH := ./source-hash.sh
SOURCE_DEFINE = -DNP_COMPOSITOR_SOURCE_HASH=\"$$($(SOURCE_HASH))\"
PREFIX      ?= /usr/local

.PHONY: all remote clean dist install print-binary shmclient vkpresent check-deps

all: remote
remote: $(BINARY)
shmclient: $(SHMCLIENT)
vkpresent: $(VKPRESENT)
dist: $(DIST_BINARY)
print-binary:
	@printf '%s\n' '$(abspath $(BINARY))'

check-deps:
	@pkg-config --exists $(PKGS) $(MEDIA_PKGS) || { \
		echo 'error: install Wayland, cJSON, xkbcommon, XCB Composite and FFmpeg development packages'; \
		exit 1; \
	}
	@command -v wayland-scanner >/dev/null || { \
		echo 'error: wayland-scanner is required'; exit 1; \
	}

$(BUILD_DIR):
	mkdir -p $@

xdg-shell-server-protocol.h:
	wayland-scanner server-header $(XDG_SHELL_XML) $@
xdg-shell-client-protocol.h:
	wayland-scanner client-header $(XDG_SHELL_XML) $@
xdg-shell-protocol.c:
	wayland-scanner private-code $(XDG_SHELL_XML) $@
viewporter-server-protocol.h:
	wayland-scanner server-header $(VIEWPORTER_XML) $@
viewporter-protocol.c:
	wayland-scanner private-code $(VIEWPORTER_XML) $@
fractional-scale-v1-server-protocol.h:
	wayland-scanner server-header $(FRACTIONAL_XML) $@
fractional-scale-v1-protocol.c:
	wayland-scanner private-code $(FRACTIONAL_XML) $@
xdg-decoration-server-protocol.h:
	wayland-scanner server-header $(DECORATION_XML) $@
xdg-decoration-protocol.c:
	wayland-scanner private-code $(DECORATION_XML) $@
text-input-v3-server-protocol.h:
	wayland-scanner server-header $(TEXT_INPUT_XML) $@
text-input-v3-client-protocol.h:
	wayland-scanner client-header $(TEXT_INPUT_XML) $@
text-input-v3-protocol.c:
	wayland-scanner private-code $(TEXT_INPUT_XML) $@
linux-dmabuf-unstable-v1-server-protocol.h:
	wayland-scanner server-header $(DMABUF_XML) $@
linux-dmabuf-unstable-v1-protocol.c:
	wayland-scanner private-code $(DMABUF_XML) $@
xwayland-shell-v1-server-protocol.h:
	wayland-scanner server-header $(XWAYLAND_XML) $@
xwayland-shell-v1-protocol.c:
	wayland-scanner private-code $(XWAYLAND_XML) $@
fifo-v1-server-protocol.h: $(FIFO_XML)
	wayland-scanner server-header $(FIFO_XML) $@
fifo-v1-protocol.c: $(FIFO_XML)
	wayland-scanner private-code $(FIFO_XML) $@

$(BINARY): $(GENERATED) $(SOURCES) $(SOURCE_HASH) Makefile | $(BUILD_DIR) check-deps
	$(CC) $(CFLAGS) $(SOURCE_DEFINE) -o $@ $(SOURCES) $(LDLIBS)

$(DIST_BINARY): $(BINARY)
	mkdir -p dist
	install -m0755 $(BINARY) $@
	strip $@

$(SHMCLIENT): xdg-shell-client-protocol.h xdg-shell-protocol.c \
	text-input-v3-client-protocol.h text-input-v3-protocol.c shmclient.c | $(BUILD_DIR)
	$(CC) -O2 -Wall -std=gnu11 $(shell pkg-config --cflags $(CLIENT_PKGS)) \
		-o $@ shmclient.c xdg-shell-protocol.c text-input-v3-protocol.c \
		$(shell pkg-config --libs $(CLIENT_PKGS))

$(VKPRESENT): xdg-shell-client-protocol.h xdg-shell-protocol.c vkpresent.c | $(BUILD_DIR)
	$(CC) -O2 -Wall -std=gnu11 $(shell pkg-config --cflags $(CLIENT_PKGS) vulkan) \
		-o $@ vkpresent.c xdg-shell-protocol.c \
		$(shell pkg-config --libs $(CLIENT_PKGS) vulkan)

install: $(BINARY)
	install -Dm0755 $(BINARY) $(DESTDIR)$(PREFIX)/bin/remotepipe-wayland

clean:
	rm -rf .build dist
