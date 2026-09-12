# One Wayland frontend with two link-selected transport/render backends. Both
# backends export np_backend_run; main.c contains no backend selection logic:
#   vmpipe:    vsock + virtio-gpu/Venus resources
#   remote: SSH stdio + immutable H.264/AV1 media resources

COMMON_PKGS   := wayland-server xkbcommon gio-unix-2.0 gdk-pixbuf-2.0 librsvg-2.0
REMOTE_PKGS   := egl glesv2 gbm libdrm libva
CLIENT_PKGS   := wayland-client
BASE_CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 -I.
STRIP         ?= strip
COMMON_CFLAGS := $(shell pkg-config --cflags $(COMMON_PKGS) 2>/dev/null)
COMMON_LIBS   := $(shell pkg-config --libs $(COMMON_PKGS) 2>/dev/null) -lm
CODEC_PREFIX ?= $(abspath ../../.build/codecs/$(ARCH)-$(LIBC))
REMOTE_CFLAGS = $(shell pkg-config --cflags $(REMOTE_PKGS) 2>/dev/null) -I$(CODEC_PREFIX)/include
REMOTE_LIBS   = $(shell pkg-config --libs egl glesv2 gbm libdrm 2>/dev/null) $(CODEC_PREFIX)/lib/libaom.a $(CODEC_PREFIX)/lib/libyuv.a -lstdc++ -lm -ldl -pthread -Wl,--gc-sections
VM_CFLAGS     := $(shell pkg-config --cflags libdrm 2>/dev/null)

PROJECT_ROOT   := $(abspath ../..)
FILE_RPC_DIR := $(PROJECT_ROOT)/common/file_rpc
FILE_RPC_SOURCES := $(FILE_RPC_DIR)/np_file_wire.c $(FILE_RPC_DIR)/np_file_server.c $(FILE_RPC_DIR)/np_file_service.c
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

ENTRY_SOURCE := main.c

FRONTEND_SOURCES := compositor.c applications.c applications_worker.c application_icons.c surface.c surface_commit.c presentation.c \
	subcompositor.c input.c decoration.c scale.c scene.c region.c perf.c \
	windowwire.c window_events.c \
	xdg_shell.c text_input.c fifo.c \
	data_device.c cursor_shape.c xwayland.c \
	cursor-shape-v1-protocol.c xdg-shell-protocol.c viewporter-protocol.c \
	fractional-scale-v1-protocol.c xdg-decoration-protocol.c \
	text-input-v3-protocol.c linux-dmabuf-unstable-v1-protocol.c \
	fifo-v1-protocol.c

BACKEND_COMMON_SOURCES := hostlink.c host_session_common.c
COMMON_SOURCES := $(ENTRY_SOURCE) $(FRONTEND_SOURCES) $(BACKEND_COMMON_SOURCES)

VM_SOURCES := backends/vmpipe/backend.c backends/vmpipe/host_session.c \
	backends/vmpipe/host_transport.c backends/vmpipe/virtio_resource.c \
	backends/vmpipe/dmabuf.c backends/vmpipe/shm_texture.c \
	backends/vmpipe/syncobj.c backends/vmpipe/vk_context.c \
	backends/vmpipe/vk_surface_buffer.c backends/vmpipe/presentation.c \
	backends/vmpipe/scene.c linux-drm-syncobj-v1-protocol.c

REMOTE_SOURCES := backends/remote/backend.c backends/remote/host_session.c \
	backends/remote/host_transport.c backends/remote/medialink.c \
	backends/remote/dmabuf.c backends/remote/dmabuf_egl.c \
	backends/remote/syncobj.c backends/remote/shm_texture.c \
	backends/remote/presentation.c backends/remote/scene.c \
	../encoder/encoder.c ../encoder/encoder_vaapi.c ../encoder/encoder_nvenc.c ../encoder/alpha.c

ARCH          ?= $(shell uname -m | sed 's/^arm64$$/aarch64/')
LIBC          ?= $(shell ldd --version 2>&1 | grep -qi musl && echo musl || echo gnu)
BUILD_DIR     := .build/$(ARCH)-$(LIBC)
FILE_RPC_LIB := $(BUILD_DIR)/file-rpc/libnativepipe-file-rpc.a
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
	$(CC) -O0 -g -Wall -Wextra -Werror -std=gnu11 -I. \
		-Ibackends/remote -o $(BUILD_DIR)/remote-transport-test \
		tests/remote_transport_test.c hostlink.c \
		backends/remote/host_transport.c backends/remote/medialink.c -pthread
	$(BUILD_DIR)/remote-transport-test

.PHONY: test-applications test-file-drag test-remote-encoder
test-remote-encoder: | $(BUILD_DIR) check-deps-remote
	$(CC) -O2 -Wall -Wextra -Werror -std=gnu11 $(REMOTE_CFLAGS) $$(pkg-config --cflags dav1d) \
		tests/remote_encoder_test.c ../encoder/encoder.c ../encoder/encoder_vaapi.c ../encoder/encoder_nvenc.c ../encoder/alpha.c \
		$(REMOTE_LIBS) $$(pkg-config --libs dav1d) -o $(BUILD_DIR)/remote-encoder-test
	$(BUILD_DIR)/remote-encoder-test
	$(CC) -O2 -Wall -Wextra -Werror -std=gnu11 $(REMOTE_CFLAGS) $$(pkg-config --cflags dav1d) \
		tests/encoder_fallback_test.c ../encoder/encoder.c ../encoder/alpha.c \
		$(REMOTE_LIBS) $$(pkg-config --libs dav1d) -o $(BUILD_DIR)/encoder-fallback-test
	$(BUILD_DIR)/encoder-fallback-test
	$(CC) -O2 -Wall -Wextra -Werror -std=gnu11 $(REMOTE_CFLAGS) $$(pkg-config --cflags dav1d) \
		tests/av1_headers_test.c $(REMOTE_LIBS) $$(pkg-config --libs dav1d) -o $(BUILD_DIR)/av1-headers-test
	$(BUILD_DIR)/av1-headers-test

.PHONY: bench-remote-encoder
.PHONY: test-hardware-encoder
test-hardware-encoder: | $(BUILD_DIR) check-deps-remote
	$(CC) -O2 -Wall -Wextra -Werror -std=gnu11 $(REMOTE_CFLAGS) \
		tests/hardware_encoder_test.c ../encoder/encoder.c ../encoder/encoder_vaapi.c ../encoder/encoder_nvenc.c ../encoder/alpha.c \
		$(REMOTE_LIBS) -o $(BUILD_DIR)/hardware-encoder-test
	$(BUILD_DIR)/hardware-encoder-test $(BUILD_DIR)/hardware.npen

BENCH_ARGS ?= 1656 1258 90
bench-remote-encoder: | $(BUILD_DIR) check-deps-remote
	$(CC) -O2 -Wall -Wextra -Werror -std=gnu11 $(REMOTE_CFLAGS) $$(pkg-config --cflags dav1d) \
		tests/remote_encoder_bench.c ../encoder/encoder.c ../encoder/encoder_vaapi.c ../encoder/encoder_nvenc.c ../encoder/alpha.c \
		$(REMOTE_LIBS) $$(pkg-config --libs dav1d) -o $(BUILD_DIR)/remote-encoder-bench
	$(BUILD_DIR)/remote-encoder-bench $(BENCH_ARGS)

test-file-drag: | $(BUILD_DIR)
	$(CC) -O0 -g -Wall -Wextra -Werror -Wno-unused-parameter -std=gnu11 -I. \
		$$(pkg-config --cflags wayland-server xkbcommon) \
		tests/file_drag_test.c windowwire.c $$(pkg-config --libs wayland-server xkbcommon) -lm -o $(BUILD_DIR)/file-drag-test
	$(BUILD_DIR)/file-drag-test

test-applications: | $(BUILD_DIR)
	$(CC) -O0 -g -Wall -Wextra -Werror -std=gnu11 -I. \
		$$(pkg-config --cflags gio-unix-2.0 gdk-pixbuf-2.0 librsvg-2.0) \
		tests/applications_test.c applications_worker.c application_icons.c windowwire.c \
		$$(pkg-config --libs gio-unix-2.0 gdk-pixbuf-2.0 librsvg-2.0) -pthread -o $(BUILD_DIR)/applications-test
	$(BUILD_DIR)/applications-test

print-binary:
	@printf '%s\n' '$(abspath $(REMOTE_BINARY))'
print-vmpipe:
	@printf '%s\n' '$(abspath $(VM_BINARY))'

check-deps: check-deps-remote check-deps-vmpipe
check-deps-remote:
	@test -s '$(CODEC_PREFIX)/lib/libaom.a' -a -s '$(CODEC_PREFIX)/lib/libyuv.a' || { \
		echo 'error: run python3 scripts/build-codecs.py $(ARCH)-$(LIBC) from the NativePipe root'; exit 1; \
	}
	@pkg-config --exists $(COMMON_PKGS) $(REMOTE_PKGS) || { \
		echo 'error: missing RemotePipe Wayland/EGL/GBM/DRM/libva development packages'; \
		exit 1; \
	}
check-deps-vmpipe:
	@pkg-config --exists $(COMMON_PKGS) libdrm || { \
		echo 'error: missing Wayland/xkbcommon/libdrm development packages'; exit 1; \
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

$(REMOTE_BINARY): $(GENERATED) $(COMMON_SOURCES) $(REMOTE_SOURCES) $(wildcard *.h backends/remote/*.h ../encoder/*.h ../encoder/vendor/*.h) \
	$(SOURCE_HASH) Makefile $(wildcard $(CODEC_PREFIX)/lib/libaom.a $(CODEC_PREFIX)/lib/libyuv.a) \
	../../scripts/build-codecs.py | $(BUILD_DIR) check-deps-remote
	$(CC) $(BASE_CFLAGS) $(COMMON_CFLAGS) $(REMOTE_CFLAGS) \
		-Ibackends/remote $(SOURCE_DEFINE) -o $@ \
		$(COMMON_SOURCES) $(REMOTE_SOURCES) $(COMMON_LIBS) $(REMOTE_LIBS)

$(FILE_RPC_LIB): $(FILE_RPC_SOURCES) $(FILE_RPC_DIR)/np_file_rpc.h $(FILE_RPC_DIR)/Makefile
	$(MAKE) -C $(FILE_RPC_DIR) CC="$(CC)" AR="$(AR)" BUILD="$(CURDIR)/$(BUILD_DIR)/file-rpc"

$(VM_BINARY): $(GENERATED) $(COMMON_SOURCES) $(VM_SOURCES) $(FILE_RPC_LIB) $(wildcard *.h backends/vmpipe/*.h) \
	$(SOURCE_HASH) Makefile | $(BUILD_DIR) check-deps-vmpipe
	$(CC) $(BASE_CFLAGS) $(COMMON_CFLAGS) $(VM_CFLAGS) $(VULKAN_CFLAGS) \
		-Ibackends/vmpipe -I$(FILE_RPC_DIR) $(SOURCE_DEFINE) -o $@ \
		$(COMMON_SOURCES) $(VM_SOURCES) $(FILE_RPC_LIB) $(COMMON_LIBS) $(VULKAN_LIBS) -pthread

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

.PHONY: test-remote-scheduler
test-remote-scheduler: | $(BUILD_DIR) check-deps-remote
	$(CC) -O0 -g -Wall -Wextra -Werror -Wno-unused-parameter -std=gnu11 -ffunction-sections -fdata-sections -I. $(COMMON_CFLAGS) $(REMOTE_CFLAGS) \
		tests/remote_scheduler_test.c -Wl,--gc-sections $(COMMON_LIBS) $(REMOTE_LIBS) -o $(BUILD_DIR)/remote-scheduler-test
	$(BUILD_DIR)/remote-scheduler-test
