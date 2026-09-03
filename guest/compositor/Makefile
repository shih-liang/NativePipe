# One Wayland protocol/state core with two transport/render backends:
#   vmpipe:    vsock + virtio-gpu/Venus resources
#   remotepipe: loopback TCP + immutable H.264 media resources

COMMON_PKGS   := wayland-server xkbcommon
REMOTE_PKGS   := egl glesv2 gbm libdrm libavcodec libavutil libswscale
CLIENT_PKGS   := wayland-client
BASE_CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11
STRIP         ?= strip
COMMON_CFLAGS := $(shell pkg-config --cflags $(COMMON_PKGS) 2>/dev/null)
COMMON_LIBS   := $(shell pkg-config --libs $(COMMON_PKGS) 2>/dev/null) -lm
REMOTE_CFLAGS := $(shell pkg-config --cflags $(REMOTE_PKGS) 2>/dev/null)
REMOTE_LIBS   := $(shell pkg-config --libs $(REMOTE_PKGS) 2>/dev/null) -pthread

PROJECT_ROOT   := $(abspath ../..)
VULKAN_HEADERS ?= $(PROJECT_ROOT)/vendor/vulkan-headers/include
ifeq ($(shell pkg-config --exists vulkan 2>/dev/null && echo yes),yes)
VULKAN_CFLAGS := $(shell pkg-config --cflags vulkan)
VULKAN_LIBS   := $(shell pkg-config --libs vulkan)
else
VULKAN_CFLAGS := -I$(VULKAN_HEADERS)
VULKAN_LIBS   := -l:libvulkan.so.1
endif

PROTOCOL_DIR   := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML  := $(PROTOCOL_DIR)/stable/xdg-shell/xdg-shell.xml
VIEWPORTER_XML := $(PROTOCOL_DIR)/stable/viewporter/viewporter.xml
FRACTIONAL_XML := $(PROTOCOL_DIR)/staging/fractional-scale/fractional-scale-v1.xml
DECORATION_XML := $(PROTOCOL_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
TEXT_INPUT_XML := $(PROTOCOL_DIR)/unstable/text-input/text-input-unstable-v3.xml
DMABUF_XML     := $(PROTOCOL_DIR)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
FIFO_XML       := fifo-v1.xml
SYNCOBJ_XML    := linux-drm-syncobj-v1.xml

GENERATED := xdg-shell-server-protocol.h xdg-shell-client-protocol.h xdg-shell-protocol.c \
	viewporter-server-protocol.h viewporter-protocol.c \
	fractional-scale-v1-server-protocol.h fractional-scale-v1-protocol.c \
	xdg-decoration-server-protocol.h xdg-decoration-protocol.c \
	text-input-v3-server-protocol.h text-input-v3-client-protocol.h \
	text-input-v3-protocol.c \
	linux-dmabuf-unstable-v1-server-protocol.h linux-dmabuf-unstable-v1-protocol.c \
	fifo-v1-server-protocol.h fifo-v1-protocol.c \
	linux-drm-syncobj-v1-server-protocol.h linux-drm-syncobj-v1-protocol.c

COMMON_SOURCES := compositor.c surface.c surface_commit.c presentation.c \
	subcompositor.c input.c decoration.c scale.c scene.c region.c perf.c \
	hostlink.c host_session_common.c windowwire.c window_events.c \
	xdg_shell.c text_input.c fifo.c \
	data_device.c cursor_shape.c xwayland.c \
	cursor-shape-v1-protocol.c xdg-shell-protocol.c viewporter-protocol.c \
	fractional-scale-v1-protocol.c xdg-decoration-protocol.c \
	text-input-v3-protocol.c linux-dmabuf-unstable-v1-protocol.c \
	fifo-v1-protocol.c

VM_SOURCES := host_session_vmpipe.c virtio_resource.c dmabuf.c shm_texture.c \
	syncobj.c linux-drm-syncobj-v1-protocol.c vk_context.c \
	vk_surface_buffer.c vmpipe_main.c

REMOTE_SOURCES := host_session_remote.c medialink.c dmabuf_remote.c \
	dmabuf_egl.c syncobj_stub.c remotepipe_main.c \
	../encoder/encoder.c ../encoder/alpha.c

ARCH          ?= $(shell uname -m | sed 's/^arm64$$/aarch64/')
LIBC          ?= $(shell ldd --version 2>&1 | grep -qi musl && echo musl || echo gnu)
BUILD_DIR     := .build/$(ARCH)-$(LIBC)
VM_BINARY     := $(BUILD_DIR)/vmpipe-wayland
REMOTE_BINARY := $(BUILD_DIR)/nativepipe-wayland
SHMCLIENT     := $(BUILD_DIR)/shmclient
VKPRESENT     := $(BUILD_DIR)/vkpresent
DIST_VM       := dist/vmpipe-wayland-$(ARCH)-$(LIBC)
DIST_REMOTE   := dist/nativepipe-wayland-$(ARCH)-$(LIBC)
SOURCE_HASH   := ./source-hash.sh
SOURCE_DEFINE  = -DNP_COMPOSITOR_SOURCE_HASH=\"$$($(SOURCE_HASH))\"
PREFIX        ?= /usr/local

.PHONY: all remote vmpipe dist dist-remote dist-vmpipe clean install \
	install-remote print-binary print-vmpipe shmclient vkpresent \
	check-deps check-deps-remote check-deps-vmpipe regenerate-protocols \
	test-remote-transport

all: remote
remote: $(REMOTE_BINARY)
vmpipe: $(VM_BINARY)
dist: dist-remote dist-vmpipe
dist-remote: $(DIST_REMOTE)
dist-vmpipe: $(DIST_VM)
shmclient: $(SHMCLIENT)
vkpresent: $(VKPRESENT)

test-remote-transport: | $(BUILD_DIR)
	$(CC) -O0 -g -Wall -Wextra -Werror -std=gnu11 -DNP_REMOTE \
		-I. -o $(BUILD_DIR)/remote-transport-test \
		tests/remote_transport_test.c hostlink.c medialink.c -pthread
	$(BUILD_DIR)/remote-transport-test

print-binary:
	@printf '%s\n' '$(abspath $(REMOTE_BINARY))'
print-vmpipe:
	@printf '%s\n' '$(abspath $(VM_BINARY))'

check-deps: check-deps-remote check-deps-vmpipe
check-deps-remote:
	@pkg-config --exists $(COMMON_PKGS) $(REMOTE_PKGS) || { \
		echo 'error: missing RemotePipe Wayland/EGL/GBM/DRM/FFmpeg development packages'; \
		exit 1; \
	}
check-deps-vmpipe:
	@pkg-config --exists $(COMMON_PKGS) || { \
		echo 'error: missing Wayland/xkbcommon development packages'; exit 1; \
	}
	@test -f '$(VULKAN_HEADERS)/vulkan/vulkan.h' || \
		pkg-config --exists vulkan || { echo 'error: Vulkan headers are required'; exit 1; }

$(BUILD_DIR):
	mkdir -p $@

regenerate-protocols:
	rm -f $(GENERATED)
	$(MAKE) $(GENERATED)

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
fifo-v1-server-protocol.h: $(FIFO_XML)
	wayland-scanner server-header $(FIFO_XML) $@
fifo-v1-protocol.c: $(FIFO_XML)
	wayland-scanner private-code $(FIFO_XML) $@
linux-drm-syncobj-v1-server-protocol.h: $(SYNCOBJ_XML)
	wayland-scanner server-header $(SYNCOBJ_XML) $@
linux-drm-syncobj-v1-protocol.c: $(SYNCOBJ_XML)
	wayland-scanner private-code $(SYNCOBJ_XML) $@

$(REMOTE_BINARY): $(GENERATED) $(COMMON_SOURCES) $(REMOTE_SOURCES) \
	$(SOURCE_HASH) Makefile | $(BUILD_DIR) check-deps-remote
	$(CC) $(BASE_CFLAGS) $(COMMON_CFLAGS) $(REMOTE_CFLAGS) \
		$(SOURCE_DEFINE) -DNP_REMOTE -o $@ \
		$(COMMON_SOURCES) $(REMOTE_SOURCES) $(COMMON_LIBS) $(REMOTE_LIBS)

$(VM_BINARY): $(GENERATED) $(COMMON_SOURCES) $(VM_SOURCES) \
	$(SOURCE_HASH) Makefile | $(BUILD_DIR) check-deps-vmpipe
	$(CC) $(BASE_CFLAGS) $(COMMON_CFLAGS) $(VULKAN_CFLAGS) \
		$(SOURCE_DEFINE) -o $@ \
		$(COMMON_SOURCES) $(VM_SOURCES) $(COMMON_LIBS) $(VULKAN_LIBS)

$(DIST_REMOTE): $(REMOTE_BINARY)
	mkdir -p dist
	install -m0755 $< $@
	$(STRIP) $@

$(DIST_VM): $(VM_BINARY)
	mkdir -p dist
	install -m0755 $< $@
	$(STRIP) $@

$(SHMCLIENT): xdg-shell-client-protocol.h xdg-shell-protocol.c \
	text-input-v3-client-protocol.h text-input-v3-protocol.c shmclient.c | $(BUILD_DIR)
	$(CC) -O2 -Wall -std=gnu11 $(shell pkg-config --cflags $(CLIENT_PKGS)) \
		-o $@ shmclient.c xdg-shell-protocol.c text-input-v3-protocol.c \
		$(shell pkg-config --libs $(CLIENT_PKGS))

$(VKPRESENT): xdg-shell-client-protocol.h xdg-shell-protocol.c vkpresent.c | $(BUILD_DIR)
	$(CC) -O2 -Wall -std=gnu11 $(shell pkg-config --cflags $(CLIENT_PKGS) vulkan) \
		-o $@ vkpresent.c xdg-shell-protocol.c \
		$(shell pkg-config --libs $(CLIENT_PKGS) vulkan)

install: $(VM_BINARY)
	install -Dm0755 $(VM_BINARY) $(DESTDIR)/usr/libexec/nativepipe/vmpipe-wayland
install-remote: $(REMOTE_BINARY)
	install -Dm0755 $(REMOTE_BINARY) $(DESTDIR)$(PREFIX)/bin/nativepipe-wayland

clean:
	rm -rf .build dist
