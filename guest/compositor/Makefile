# Built inside the guest / remote Linux host.
#
# VM build: one compositor-owned Venus VkDevice used only for wl_shm upload
# textures. Client GPU windows retain their original linux-dmabuf textures;
# there is no manual virtio CONTEXT_INIT/blob allocator.
# Remote build keeps the existing encoded transport and does not link Vulkan.

PKGS          := wayland-server libcjson xkbcommon
CLIENT_PKGS   := wayland-client
CFLAGS        += -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(shell pkg-config --cflags $(PKGS))
LDLIBS        += $(shell pkg-config --libs $(PKGS))
PROJECT_ROOT  := $(abspath ../../../..)
ifeq ($(shell pkg-config --exists vulkan 2>/dev/null && echo yes),yes)
VULKAN_CFLAGS := $(shell pkg-config --cflags vulkan)
VULKAN_LIBS   := $(shell pkg-config --libs vulkan)
else
# Runtime-only guest images intentionally omit development packages. During a
# source-tree test, use the pinned Vulkan headers and the loader SONAME already
# installed with Mesa instead of mutating the guest on every build.
VULKAN_CFLAGS := -I$(PROJECT_ROOT)/Modules/MoltenVK/External/Vulkan-Headers/include
VULKAN_LIBS   := -l:libvulkan.so.1
endif

FFMPEG_PKGS   := libavcodec libavutil libswscale
FFMPEG_CFLAGS := $(shell pkg-config --cflags $(FFMPEG_PKGS) 2>/dev/null)
FFMPEG_LIBS   := $(shell pkg-config --libs $(FFMPEG_PKGS) 2>/dev/null)

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

SHARED_SOURCES := compositor.c surface.c surface_commit.c presentation.c \
                  subcompositor.c input.c host_session.c \
                  decoration.c scale.c scene.c region.c perf.c hostlink.c \
                  windowwire.c window_events.c xdg_shell.c text_input.c fifo.c data_device.c \
                  cursor_shape.c cursor-shape-v1-protocol.c \
                  xdg-shell-protocol.c viewporter-protocol.c fractional-scale-v1-protocol.c \
                  xdg-decoration-protocol.c text-input-v3-protocol.c \
                  linux-dmabuf-unstable-v1-protocol.c fifo-v1-protocol.c

# linux-dmabuf remains in the client's existing Venus texture. Only wl_shm
# needs compositor-owned Vulkan storage, one texture per wl_buffer.
VM_SOURCES := virtio_resource.c dmabuf.c shm_texture.c \
              syncobj.c linux-drm-syncobj-v1-protocol.c \
              vk_context.c vk_surface_buffer.c vmpipe_main.c

REMOTE_SOURCES := medialink.c ../encoder/encoder.c dmabuf_stub.c syncobj_stub.c \
                  remotepipe_main.c

BINARY        := vmpipe-wayland
REMOTE_BINARY := remotepipe-wayland
CLIENT        := shmclient
VKPRESENT     := vkpresent
ALIGN_BLOB    := ../align-host-blob.so
DISCONNECT_TRACE := ../tools/wayland-disconnect-trace.so

.PHONY: all remote clean install install-remote align-blob disconnect-trace static static-clean

all: $(BINARY) $(CLIENT)
remote: $(REMOTE_BINARY)
align-blob: $(ALIGN_BLOB)
disconnect-trace: $(DISCONNECT_TRACE)

$(ALIGN_BLOB): ../align-host-blob.c
	$(CC) -O2 -shared -fPIC -o $@ $< -ldl

$(DISCONNECT_TRACE): ../tools/wayland-disconnect-trace.c
	$(CC) -O0 -g -Wall -Wextra -shared -fPIC -o $@ $< -ldl

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

$(BINARY): $(GENERATED) $(SHARED_SOURCES) $(VM_SOURCES)
	$(CC) $(CFLAGS) $(VULKAN_CFLAGS) -o $@ \
		$(SHARED_SOURCES) $(VM_SOURCES) $(LDLIBS) $(VULKAN_LIBS)

$(REMOTE_BINARY): $(GENERATED) $(SHARED_SOURCES) $(REMOTE_SOURCES)
	@if [ -z "$(FFMPEG_LIBS)" ]; then \
		echo "error: ffmpeg pkg-config missing (need libavcodec libavutil libswscale)"; exit 1; \
	fi
	$(CC) $(CFLAGS) $(FFMPEG_CFLAGS) -DNP_REMOTE -o $@ \
		$(SHARED_SOURCES) $(REMOTE_SOURCES) $(LDLIBS) $(FFMPEG_LIBS)

$(CLIENT): xdg-shell-client-protocol.h xdg-shell-protocol.c \
           text-input-v3-client-protocol.h text-input-v3-protocol.c shmclient.c
	$(CC) -O2 -Wall -std=gnu11 $(shell pkg-config --cflags $(CLIENT_PKGS)) \
		-o $@ shmclient.c xdg-shell-protocol.c text-input-v3-protocol.c \
		$(shell pkg-config --libs $(CLIENT_PKGS))

$(VKPRESENT): xdg-shell-client-protocol.h xdg-shell-protocol.c vkpresent.c
	$(CC) -O2 -Wall -std=gnu11 $(shell pkg-config --cflags $(CLIENT_PKGS) vulkan) \
		-o $@ vkpresent.c xdg-shell-protocol.c \
		$(shell pkg-config --libs $(CLIENT_PKGS) vulkan)

install: $(BINARY)
	install -Dm0755 $(BINARY) /usr/libexec/nativepipe/$(BINARY)
install-remote: $(REMOTE_BINARY)
	install -Dm0755 $(REMOTE_BINARY) /usr/libexec/nativepipe/$(REMOTE_BINARY)

clean:
	rm -f $(BINARY) $(REMOTE_BINARY) $(CLIENT) $(VKPRESENT) $(ALIGN_BLOB) \
	      $(DISCONNECT_TRACE) $(GENERATED) $(SCENE_SHADER_OBJECTS)

# The VM compositor now dynamically uses the guest Vulkan loader/Venus ICD.
# The former fully-static musl build embedded the deleted blob allocator and is
# intentionally retired rather than silently producing an obsolete binary.
static:
	@echo "vmpipe static build retired: Vulkan-native compositor requires the guest Vulkan loader/Venus ICD" >&2
	@false

static-clean:
	rm -rf dist .deps
