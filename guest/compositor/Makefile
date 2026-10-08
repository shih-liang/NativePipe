# One Wayland frontend with two link-selected transport/render backends. Both
# backends export np_backend_run; main.c contains no backend selection logic:
#   vmpipe:    vsock + virtio-gpu/Venus resources
#   remote: SSH stdio + immutable H.264/AV1 media resources

ARCH          ?= $(shell uname -m | sed 's/^arm64$$/aarch64/')
LIBC          ?= $(shell ldd --version 2>&1 | grep -qi musl && echo musl || echo gnu)

WAYLAND_PREFIX := $(abspath ../../.build/wayland/$(ARCH)-$(LIBC))
export PKG_CONFIG_PATH := $(WAYLAND_PREFIX)/lib/pkgconfig:$(PKG_CONFIG_PATH)
PKG_CONFIG := env PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config
WAYLAND_SCANNER := $(WAYLAND_PREFIX)/bin/wayland-scanner
PROTOCOL_DIR := $(WAYLAND_PREFIX)/share/wayland-protocols
WAYLAND_STAMP := $(WAYLAND_PREFIX)/.complete
WAYLAND_BUILD := ../../scripts/build-wayland.py
WAYLAND_SERVER_ARCHIVE := $(WAYLAND_PREFIX)/lib/libwayland-server.a
WAYLAND_CLIENT_ARCHIVE := $(WAYLAND_PREFIX)/lib/libwayland-client.a

COMMON_PKGS   := wayland-server xkbcommon gio-unix-2.0 gdk-pixbuf-2.0 librsvg-2.0
REMOTE_PKGS   := egl glesv2 gbm libdrm libva
CLIENT_PKGS   := wayland-client
PRODUCT_VERSION_FILE := $(abspath ../../Sources/NativePipeStrings/Resources/VERSION)
PRODUCT_VERSION := $(shell cat $(PRODUCT_VERSION_FILE))
PRODUCT_VERSION_DEFINE := -DNATIVEPIPE_PRODUCT_VERSION=\"$(PRODUCT_VERSION)\"
BASE_CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 -I. $(PRODUCT_VERSION_DEFINE)
STRIP         ?= strip
COMMON_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(COMMON_PKGS) 2>/dev/null)
WAYLAND_SERVER_LIBS = $(WAYLAND_SERVER_ARCHIVE) $(filter-out -lwayland-server,$(shell $(PKG_CONFIG) --static --libs wayland-server 2>/dev/null))
WAYLAND_CLIENT_LIBS = $(WAYLAND_CLIENT_ARCHIVE) $(filter-out -lwayland-client,$(shell $(PKG_CONFIG) --static --libs wayland-client 2>/dev/null))
COMMON_LIBS = $(shell $(PKG_CONFIG) --libs $(filter-out wayland-server,$(COMMON_PKGS)) 2>/dev/null) $(WAYLAND_SERVER_LIBS) -lm
CODEC_PREFIX ?= $(abspath ../../.build/codecs/$(ARCH)-$(LIBC))
REMOTE_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(REMOTE_PKGS) 2>/dev/null) -I$(CODEC_PREFIX)/include
REMOTE_LIBS   = $(shell $(PKG_CONFIG) --libs egl glesv2 gbm libdrm 2>/dev/null) $(CODEC_PREFIX)/lib/libaom.a $(CODEC_PREFIX)/lib/libyuv.a -lstdc++ -lm -ldl -pthread -Wl,--gc-sections
VM_CFLAGS     = $(shell $(PKG_CONFIG) --cflags libdrm 2>/dev/null)

PROJECT_ROOT   := $(abspath ../..)
FILE_RPC_DIR := $(PROJECT_ROOT)/common/file_rpc
FILE_RPC_SOURCES := $(FILE_RPC_DIR)/np_file_wire.c $(FILE_RPC_DIR)/np_file_server.c $(FILE_RPC_DIR)/np_file_service.c
VULKAN_HEADERS ?= $(PROJECT_ROOT)/vendor/vulkan-headers/include
ifeq ($(shell $(PKG_CONFIG) --exists vulkan 2>/dev/null && echo yes),yes)
VULKAN_CFLAGS := $(shell $(PKG_CONFIG) --cflags vulkan)
VULKAN_LIBS   := $(shell $(PKG_CONFIG) --libs vulkan)
else
VULKAN_CFLAGS := -I$(VULKAN_HEADERS)
VULKAN_LIBS   := -l:libvulkan.so.1
endif

XDG_SHELL_XML  := $(PROTOCOL_DIR)/stable/xdg-shell/xdg-shell.xml
VIEWPORTER_XML := $(PROTOCOL_DIR)/stable/viewporter/viewporter.xml
FRACTIONAL_XML := $(PROTOCOL_DIR)/staging/fractional-scale/fractional-scale-v1.xml
DECORATION_XML := $(PROTOCOL_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
TEXT_INPUT_XML := $(PROTOCOL_DIR)/unstable/text-input/text-input-unstable-v3.xml
DMABUF_XML     := $(PROTOCOL_DIR)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
FIFO_XML       := fifo-v1.xml
SYNCOBJ_XML    := linux-drm-syncobj-v1.xml
ACTIVATION_XML := xdg-activation-v1.xml
XDG_OUTPUT_XML := xdg-output-unstable-v1.xml
PRESENTATION_XML := $(PROTOCOL_DIR)/stable/presentation-time/presentation-time.xml

GENERATED := xdg-shell-server-protocol.h xdg-shell-client-protocol.h xdg-shell-protocol.c \
	viewporter-server-protocol.h viewporter-protocol.c \
	fractional-scale-v1-server-protocol.h fractional-scale-v1-protocol.c \
	xdg-decoration-server-protocol.h xdg-decoration-protocol.c \
	text-input-v3-server-protocol.h text-input-v3-client-protocol.h \
	text-input-v3-protocol.c \
	linux-dmabuf-unstable-v1-server-protocol.h linux-dmabuf-unstable-v1-protocol.c \
	fifo-v1-server-protocol.h fifo-v1-protocol.c \
	linux-drm-syncobj-v1-server-protocol.h linux-drm-syncobj-v1-protocol.c \
	xdg-activation-v1-server-protocol.h xdg-activation-v1-protocol.c \
	xdg-output-unstable-v1-server-protocol.h xdg-output-unstable-v1-protocol.c \
	presentation-time-server-protocol.h presentation-time-protocol.c

ENTRY_SOURCE := main.c

FRONTEND_SOURCES := compositor.c activation.c presentation_time.c applications.c applications_worker.c application_icons.c notifications.c notify_dbus.c host_open.c surface.c surface_commit.c presentation.c \
	subcompositor.c input.c keymap.c decoration.c scale.c scene.c region.c perf.c \
	windowwire.c window_events.c \
	xdg_shell.c text_input.c fifo.c \
	data_device.c cursor_shape.c xwayland.c user_text.c \
	cursor-shape-v1-protocol.c xdg-shell-protocol.c viewporter-protocol.c \
	fractional-scale-v1-protocol.c xdg-decoration-protocol.c \
	text-input-v3-protocol.c linux-dmabuf-unstable-v1-protocol.c \
	fifo-v1-protocol.c xdg-activation-v1-protocol.c xdg-output-unstable-v1-protocol.c presentation-time-protocol.c

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

.PHONY: test-applications test-file-drag test-notify-dbus test-host-open test-xwayland-session test-remote-encoder test-scene-damage test-user-text
test-xwayland-session: | $(BUILD_DIR)
	$(CC) -O0 -g -Wall -Wextra -Werror -std=gnu11 -I. \
		$$(pkg-config --cflags wayland-server) tests/xwayland_session_test.c user_text.c \
		$(WAYLAND_SERVER_LIBS) -o $(BUILD_DIR)/xwayland-session-test
	$(BUILD_DIR)/xwayland-session-test

test-host-open: | $(BUILD_DIR)
	$(CC) -O0 -g -Wall -Wextra -Werror -std=gnu11 -I. \
		$$(pkg-config --cflags wayland-server glib-2.0) \
		tests/host_open_test.c host_open.c windowwire.c window_events.c \
		$(WAYLAND_SERVER_LIBS) $$(pkg-config --libs glib-2.0) -o $(BUILD_DIR)/host-open-test
	$(BUILD_DIR)/host-open-test

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
		tests/file_drag_test.c windowwire.c user_text.c $(WAYLAND_SERVER_LIBS) $$(pkg-config --libs xkbcommon) -lm -o $(BUILD_DIR)/file-drag-test
	$(BUILD_DIR)/file-drag-test

# The damage contract between scene construction and the host's drawable pool.
# Needs only libwayland: the backends are fixtures, so it runs anywhere the
# compositor builds, without a GPU or a host session.
test-scene-damage: | $(BUILD_DIR)
	$(CC) -O0 -g -Wall -Wextra -Werror -Wno-unused-parameter -std=gnu11 -I. \
		$$(pkg-config --cflags wayland-server) \
		tests/scene_damage_test.c $(WAYLAND_SERVER_LIBS) -lm \
		-o $(BUILD_DIR)/scene-damage-test
	$(BUILD_DIR)/scene-damage-test

# Messages printed for the user arrive translated through the environment, so
# the text must never be interpreted as a printf format. libc only.
test-user-text: | $(BUILD_DIR)
	$(CC) -O0 -g -Wall -Wextra -Werror -std=gnu11 -I. \
		tests/user_text_test.c user_text.c -o $(BUILD_DIR)/user-text-test
	$(BUILD_DIR)/user-text-test

# org.freedesktop.Notifications over a point-to-point D-Bus connection: real
# method calls and signals with no bus daemon and no Wayland, so GIO is enough.
test-notify-dbus: $(PRODUCT_VERSION_FILE) | $(BUILD_DIR)
	$(CC) -O0 -g -Wall -Wextra -Werror -std=gnu11 -I. \
		$(PRODUCT_VERSION_DEFINE) $$(pkg-config --cflags gio-unix-2.0) \
		tests/notify_dbus_test.c notify_dbus.c $$(pkg-config --libs gio-unix-2.0) \
		-o $(BUILD_DIR)/notify-dbus-test
	$(BUILD_DIR)/notify-dbus-test
	$(CC) -O0 -g -Wall -Wextra -Werror -std=gnu11 -I. \
		$(PRODUCT_VERSION_DEFINE) -DNP_NOTIFICATIONS_LIFECYCLE_TEST \
		$$(pkg-config --cflags gio-unix-2.0 wayland-server) \
		tests/notify_dbus_test.c notify_dbus.c windowwire.c \
		$$(pkg-config --libs gio-unix-2.0) $(WAYLAND_SERVER_LIBS) \
		-o $(BUILD_DIR)/notifications-lifecycle-test
	$(BUILD_DIR)/notifications-lifecycle-test
	dbus-run-session -- $(BUILD_DIR)/notifications-lifecycle-test --session-bus

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
check-deps-remote: wayland-deps
	@test -s '$(CODEC_PREFIX)/lib/libaom.a' -a -s '$(CODEC_PREFIX)/lib/libyuv.a' || { \
		echo 'error: run python3 scripts/build-codecs.py $(ARCH)-$(LIBC) from the NativePipe root'; exit 1; \
	}
	@pkg-config --exists $(COMMON_PKGS) $(REMOTE_PKGS) || { \
		echo 'error: missing RemotePipe Wayland/EGL/GBM/DRM/libva development packages'; \
		exit 1; \
	}
check-deps-vmpipe: wayland-deps
	@pkg-config --exists $(COMMON_PKGS) libdrm || { \
		echo 'error: missing Wayland/xkbcommon/libdrm development packages'; exit 1; \
	}
	@test -f '$(VULKAN_HEADERS)/vulkan/vulkan.h' || \
		pkg-config --exists vulkan || { echo 'error: Vulkan headers are required'; exit 1; }

$(BUILD_DIR):
	mkdir -p $@

regenerate-protocols: wayland-deps
	rm -f $(GENERATED)
	$(MAKE) $(GENERATED)

xdg-shell-server-protocol.h: $(XDG_SHELL_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) server-header $(XDG_SHELL_XML) $@
xdg-shell-client-protocol.h: $(XDG_SHELL_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) client-header $(XDG_SHELL_XML) $@
xdg-shell-protocol.c: $(XDG_SHELL_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL_XML) $@
viewporter-server-protocol.h: $(VIEWPORTER_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) server-header $(VIEWPORTER_XML) $@
viewporter-protocol.c: $(VIEWPORTER_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) private-code $(VIEWPORTER_XML) $@
fractional-scale-v1-server-protocol.h: $(FRACTIONAL_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) server-header $(FRACTIONAL_XML) $@
fractional-scale-v1-protocol.c: $(FRACTIONAL_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) private-code $(FRACTIONAL_XML) $@
xdg-decoration-server-protocol.h: $(DECORATION_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) server-header $(DECORATION_XML) $@
xdg-decoration-protocol.c: $(DECORATION_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) private-code $(DECORATION_XML) $@
text-input-v3-server-protocol.h: $(TEXT_INPUT_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) server-header $(TEXT_INPUT_XML) $@
text-input-v3-client-protocol.h: $(TEXT_INPUT_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) client-header $(TEXT_INPUT_XML) $@
text-input-v3-protocol.c: $(TEXT_INPUT_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) private-code $(TEXT_INPUT_XML) $@
linux-dmabuf-unstable-v1-server-protocol.h: $(DMABUF_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) server-header $(DMABUF_XML) $@
linux-dmabuf-unstable-v1-protocol.c: $(DMABUF_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) private-code $(DMABUF_XML) $@
fifo-v1-server-protocol.h: $(FIFO_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) server-header $(FIFO_XML) $@
fifo-v1-protocol.c: $(FIFO_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) private-code $(FIFO_XML) $@
linux-drm-syncobj-v1-server-protocol.h: $(SYNCOBJ_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) server-header $(SYNCOBJ_XML) $@
linux-drm-syncobj-v1-protocol.c: $(SYNCOBJ_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) private-code $(SYNCOBJ_XML) $@

$(REMOTE_BINARY): $(WAYLAND_STAMP) $(WAYLAND_SERVER_ARCHIVE) $(GENERATED) $(COMMON_SOURCES) $(REMOTE_SOURCES) $(wildcard *.h backends/remote/*.h ../encoder/*.h ../encoder/vendor/*.h) \
	../session/np-open-wire.h \
	$(SOURCE_HASH) Makefile $(PRODUCT_VERSION_FILE) $(wildcard $(CODEC_PREFIX)/lib/libaom.a $(CODEC_PREFIX)/lib/libyuv.a) \
	../../scripts/build-codecs.py ../../scripts/build-wayland.py | $(BUILD_DIR) check-deps-remote
	$(CC) $(BASE_CFLAGS) $(COMMON_CFLAGS) $(REMOTE_CFLAGS) \
		-Ibackends/remote $(SOURCE_DEFINE) -o $@ \
		$(COMMON_SOURCES) $(REMOTE_SOURCES) $(COMMON_LIBS) $(REMOTE_LIBS) -Wl,--exclude-libs,libwayland-server.a

$(FILE_RPC_LIB): $(FILE_RPC_SOURCES) $(FILE_RPC_DIR)/np_file_rpc.h $(FILE_RPC_DIR)/Makefile
	$(MAKE) -C $(FILE_RPC_DIR) CC="$(CC)" AR="$(AR)" BUILD="$(CURDIR)/$(BUILD_DIR)/file-rpc"

$(VM_BINARY): $(WAYLAND_STAMP) $(WAYLAND_SERVER_ARCHIVE) $(GENERATED) $(COMMON_SOURCES) $(VM_SOURCES) $(FILE_RPC_LIB) $(wildcard *.h backends/vmpipe/*.h) \
	../session/np-open-wire.h \
	$(SOURCE_HASH) Makefile $(PRODUCT_VERSION_FILE) | $(BUILD_DIR) check-deps-vmpipe
	$(CC) $(BASE_CFLAGS) $(COMMON_CFLAGS) $(VM_CFLAGS) $(VULKAN_CFLAGS) \
		-Ibackends/vmpipe -I$(FILE_RPC_DIR) $(SOURCE_DEFINE) -o $@ \
		$(COMMON_SOURCES) $(VM_SOURCES) $(FILE_RPC_LIB) $(COMMON_LIBS) $(VULKAN_LIBS) -pthread -Wl,--exclude-libs,libwayland-server.a

$(DIST_REMOTE): $(REMOTE_BINARY)
	mkdir -p dist
	install -m0755 $< $@
	$(STRIP) $@

$(DIST_VM): $(VM_BINARY)
	mkdir -p dist
	install -m0755 $< $@
	$(STRIP) $@

$(SHMCLIENT): $(WAYLAND_STAMP) $(WAYLAND_CLIENT_ARCHIVE) xdg-shell-client-protocol.h xdg-shell-protocol.c \
	text-input-v3-client-protocol.h text-input-v3-protocol.c shmclient.c | $(BUILD_DIR)
	$(CC) -O2 -Wall -std=gnu11 $(shell $(PKG_CONFIG) --cflags $(CLIENT_PKGS)) \
		-o $@ shmclient.c xdg-shell-protocol.c text-input-v3-protocol.c \
		$(WAYLAND_CLIENT_LIBS)

$(VKPRESENT): $(WAYLAND_STAMP) $(WAYLAND_CLIENT_ARCHIVE) xdg-shell-client-protocol.h xdg-shell-protocol.c vkpresent.c | $(BUILD_DIR)
	$(CC) -O2 -Wall -std=gnu11 $(shell $(PKG_CONFIG) --cflags $(CLIENT_PKGS) vulkan) \
		-o $@ vkpresent.c xdg-shell-protocol.c \
		$(WAYLAND_CLIENT_LIBS) $(shell $(PKG_CONFIG) --libs vulkan)

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

.PHONY: wayland-deps wayland-cache-check
wayland-deps: $(WAYLAND_STAMP)

# Validate restored caches before pkg-config can fall back to system Wayland.
# The builder leaves the stamp unchanged when every pinned artifact is intact.
wayland-cache-check:
$(WAYLAND_STAMP): $(WAYLAND_BUILD) wayland-cache-check
	python3 $(WAYLAND_BUILD) $(ARCH)-$(LIBC)

$(WAYLAND_SCANNER) $(WAYLAND_SERVER_ARCHIVE) $(WAYLAND_CLIENT_ARCHIVE): $(WAYLAND_STAMP)
	@test -s $@ || python3 $(WAYLAND_BUILD) $(ARCH)-$(LIBC)

$(GENERATED) $(SHMCLIENT) $(VKPRESENT) test-xwayland-session test-host-open test-file-drag test-scene-damage test-notify-dbus: | wayland-deps

xdg-activation-v1-server-protocol.h: $(ACTIVATION_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) server-header $< $@
xdg-activation-v1-protocol.c: $(ACTIVATION_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) private-code $< $@
xdg-output-unstable-v1-server-protocol.h: $(XDG_OUTPUT_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) server-header $< $@
xdg-output-unstable-v1-protocol.c: $(XDG_OUTPUT_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) private-code $< $@
presentation-time-server-protocol.h: $(PRESENTATION_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) server-header $(PRESENTATION_XML) $@
presentation-time-protocol.c: $(PRESENTATION_XML) $(WAYLAND_SCANNER) $(WAYLAND_STAMP)
	$(WAYLAND_SCANNER) private-code $(PRESENTATION_XML) $@

.PHONY: test-core-protocols test-input-protocol test-presentation-time test-scene-admission test-flat-feedback test-vmpipe-syncobj test-vmpipe-dmabuf
PROTOCOL_TEST_CFLAGS = -O0 -g -Wall -Wextra -Werror -Wno-unused-parameter -std=gnu11 -ffunction-sections -fdata-sections -I. $(COMMON_CFLAGS)
test-core-protocols: $(GENERATED) | $(BUILD_DIR) wayland-deps
	$(CC) $(PROTOCOL_TEST_CFLAGS) tests/core_protocol_test.c windowwire.c xdg-shell-protocol.c $(COMMON_LIBS) -Wl,--gc-sections -o $(BUILD_DIR)/core-protocol-test
	$(BUILD_DIR)/core-protocol-test
test-input-protocol: $(GENERATED) | $(BUILD_DIR) wayland-deps
	$(CC) $(PROTOCOL_TEST_CFLAGS) tests/input_protocol_test.c $(COMMON_LIBS) -Wl,--gc-sections -o $(BUILD_DIR)/input-protocol-test
	$(BUILD_DIR)/input-protocol-test
test-presentation-time: $(GENERATED) | $(BUILD_DIR) wayland-deps
	$(CC) $(PROTOCOL_TEST_CFLAGS) tests/presentation_time_test.c presentation-time-protocol.c $(COMMON_LIBS) -o $(BUILD_DIR)/presentation-time-test
	$(BUILD_DIR)/presentation-time-test
	$(CC) -std=gnu11 -Wall -Wextra -Werror tests/presentation_clock_test.c -o $(BUILD_DIR)/presentation-clock-test
	$(BUILD_DIR)/presentation-clock-test
test-scene-admission: $(GENERATED) | $(BUILD_DIR) wayland-deps
	$(CC) $(PROTOCOL_TEST_CFLAGS) $(REMOTE_CFLAGS) tests/scene_admission_test.c presentation-time-protocol.c -Wl,--gc-sections $(COMMON_LIBS) -o $(BUILD_DIR)/scene-admission-test
	$(BUILD_DIR)/scene-admission-test
test-flat-feedback: $(GENERATED) | $(BUILD_DIR) wayland-deps
	$(CC) $(PROTOCOL_TEST_CFLAGS) tests/flat_commit_feedback_test.c presentation-time-protocol.c -Wl,--gc-sections $(COMMON_LIBS) -o $(BUILD_DIR)/flat-commit-feedback-test
	$(BUILD_DIR)/flat-commit-feedback-test
test-vmpipe-syncobj: $(GENERATED) | $(BUILD_DIR) wayland-deps
	$(CC) $(PROTOCOL_TEST_CFLAGS) $(VM_CFLAGS) $(VULKAN_CFLAGS) -Ibackends/vmpipe -I$(FILE_RPC_DIR) tests/vmpipe_syncobj_test.c linux-drm-syncobj-v1-protocol.c $(COMMON_LIBS) -Wl,--gc-sections -o $(BUILD_DIR)/vmpipe-syncobj-test
	$(BUILD_DIR)/vmpipe-syncobj-test
test-vmpipe-dmabuf: $(GENERATED) | $(BUILD_DIR) wayland-deps
	$(CC) $(PROTOCOL_TEST_CFLAGS) $(VM_CFLAGS) tests/vmpipe_dmabuf_test.c linux-dmabuf-unstable-v1-protocol.c $(COMMON_LIBS) -Wl,--gc-sections -o $(BUILD_DIR)/vmpipe-dmabuf-test
	$(BUILD_DIR)/vmpipe-dmabuf-test

.PHONY: test-activation test-xdg-output
test-activation: $(GENERATED) | $(BUILD_DIR) wayland-deps
	$(CC) $(PROTOCOL_TEST_CFLAGS) tests/activation_test.c windowwire.c xdg-activation-v1-protocol.c $(COMMON_LIBS) -o $(BUILD_DIR)/activation-test
	$(BUILD_DIR)/activation-test
test-xdg-output: $(GENERATED) | $(BUILD_DIR) wayland-deps
	$(CC) $(PROTOCOL_TEST_CFLAGS) tests/xdg_output_test.c viewporter-protocol.c fractional-scale-v1-protocol.c xdg-output-unstable-v1-protocol.c $(COMMON_LIBS) -o $(BUILD_DIR)/xdg-output-test
	$(BUILD_DIR)/xdg-output-test

$(XDG_SHELL_XML) $(VIEWPORTER_XML) $(FRACTIONAL_XML) $(DECORATION_XML) $(TEXT_INPUT_XML) $(DMABUF_XML) $(PRESENTATION_XML): $(WAYLAND_STAMP)
	@test -s $@ || python3 $(WAYLAND_BUILD) $(ARCH)-$(LIBC)

.PHONY: test-text-input
test-text-input: $(GENERATED) | $(BUILD_DIR) wayland-deps
	$(CC) $(PROTOCOL_TEST_CFLAGS) tests/text_input_test.c windowwire.c text-input-v3-protocol.c $(COMMON_LIBS) -o $(BUILD_DIR)/text-input-test
	$(BUILD_DIR)/text-input-test
