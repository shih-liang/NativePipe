# Built inside the guest / remote Linux host.
#
# Two compile-time products (not a runtime flag):
#
#   make            → vmpipe-wayland
#                     LightHouse VM: vsock + virtio-gpu blobs / Venus dmabuf
#
#   make remote     → remotepipe-wayland
#                     Bare-metal remote: TCP 1025/1026 + H.264 encode (no virtio)
#
# Shared Wayland core lives in compositor.c (+ hostlink, protocols).
# Thin mains: vmpipe_main.c / remotepipe_main.c.
# Remote does NOT link blob.c / dmabuf.c.
#
# VM guest deps:
#   apk add build-base linux-headers wayland-dev wayland-protocols cjson-dev \
#           libxkbcommon-dev
#
# Remote host deps (additional):
#   apk add ffmpeg-dev   # or libavcodec-dev libavutil-dev libswscale-dev
#   # optional VA-API: libva-dev + a VA driver

PKGS          := wayland-server libcjson xkbcommon
CLIENT_PKGS   := wayland-client
CFLAGS        += -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(shell pkg-config --cflags $(PKGS))
LDLIBS        += $(shell pkg-config --libs $(PKGS))

FFMPEG_PKGS   := libavcodec libavutil libswscale
FFMPEG_CFLAGS := $(shell pkg-config --cflags $(FFMPEG_PKGS) 2>/dev/null)
FFMPEG_LIBS   := $(shell pkg-config --libs $(FFMPEG_PKGS) 2>/dev/null)

PROTOCOL_DIR  := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML := $(PROTOCOL_DIR)/stable/xdg-shell/xdg-shell.xml
FRACTIONAL_XML := $(PROTOCOL_DIR)/staging/fractional-scale/fractional-scale-v1.xml
DECORATION_XML := $(PROTOCOL_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
TEXT_INPUT_XML := $(PROTOCOL_DIR)/unstable/text-input/text-input-unstable-v3.xml
DMABUF_XML    := $(PROTOCOL_DIR)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml

GENERATED     := xdg-shell-server-protocol.h xdg-shell-client-protocol.h xdg-shell-protocol.c \
                 fractional-scale-v1-server-protocol.h fractional-scale-v1-protocol.c \
                 xdg-decoration-server-protocol.h xdg-decoration-protocol.c \
                 text-input-v3-server-protocol.h text-input-v3-client-protocol.h \
                 text-input-v3-protocol.c \
                 linux-dmabuf-unstable-v1-server-protocol.h linux-dmabuf-unstable-v1-protocol.c

# Shared Wayland / protocol / hostlink (both builds).
SHARED_SOURCES := compositor.c hostlink.c \
                 xdg-shell-protocol.c fractional-scale-v1-protocol.c \
                 xdg-decoration-protocol.c text-input-v3-protocol.c \
                 linux-dmabuf-unstable-v1-protocol.c

# VM-only: virtio blob mapping + linux-dmabuf advertise helpers.
VM_SOURCES    := blob.c dmabuf.c vmpipe_main.c

# Remote-only: media TCP + FFmpeg encoder + dmabuf stub + thin main (no virtio blob/dmabuf).
REMOTE_SOURCES := medialink.c ../encoder/encoder.c dmabuf_stub.c remotepipe_main.c

BINARY        := vmpipe-wayland
REMOTE_BINARY := remotepipe-wayland
CLIENT        := shmclient
VKPRESENT     := vkpresent
ALIGN_BLOB    := ../align-host-blob.so

.PHONY: all remote clean install install-remote align-blob static static-clean

all: $(BINARY) $(CLIENT)

remote: $(REMOTE_BINARY)

align-blob: $(ALIGN_BLOB)

$(ALIGN_BLOB): ../align-host-blob.c
	$(CC) -O2 -shared -fPIC -o $@ $< -ldl

xdg-shell-server-protocol.h:
	wayland-scanner server-header $(XDG_SHELL_XML) $@

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(XDG_SHELL_XML) $@

xdg-shell-protocol.c:
	wayland-scanner private-code $(XDG_SHELL_XML) $@

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

# LightHouse VM compositor: vsock + virtio blobs. No FFmpeg.
$(BINARY): $(GENERATED) $(SHARED_SOURCES) $(VM_SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SHARED_SOURCES) $(VM_SOURCES) $(LDLIBS)

# Remote bare-metal compositor: TCP + H.264. No virtio blob/dmabuf objects.
$(REMOTE_BINARY): $(GENERATED) $(SHARED_SOURCES) $(REMOTE_SOURCES)
	@if [ -z "$(FFMPEG_LIBS)" ]; then \
		echo "error: ffmpeg pkg-config missing (need libavcodec libavutil libswscale)"; \
		exit 1; \
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
	rm -f $(BINARY) $(REMOTE_BINARY) $(CLIENT) $(VKPRESENT) $(ALIGN_BLOB) $(GENERATED)
	rm -f nativepipe-wayland nativepipe-wayland-remote

## Host-side static musl builds (zig). Output: dist/vmpipe-wayland-{aarch64,x86_64}
static:
	chmod +x build-static.sh
	./build-static.sh aarch64-linux-musl
	./build-static.sh x86_64-linux-musl

static-clean:
	rm -rf dist .deps
