# Built inside the guest.
#
#   apk add build-base linux-headers wayland-dev wayland-protocols cjson-dev \
#           libxkbcommon-dev
#   make

PKGS          := wayland-server libcjson xkbcommon
CLIENT_PKGS   := wayland-client
CFLAGS        += -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(shell pkg-config --cflags $(PKGS))
LDLIBS        += $(shell pkg-config --libs $(PKGS))

PROTOCOL_DIR  := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML := $(PROTOCOL_DIR)/stable/xdg-shell/xdg-shell.xml
# Per-surface scale. wl_output's scale is global, which cannot describe two
# windows on displays of different density.
FRACTIONAL_XML := $(PROTOCOL_DIR)/staging/fractional-scale/fractional-scale-v1.xml
DECORATION_XML := $(PROTOCOL_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
# The IME path. macOS composes; the guest client only ever sees finished text
# and a preedit string, never the keystrokes that produced them.
TEXT_INPUT_XML := $(PROTOCOL_DIR)/unstable/text-input/text-input-unstable-v3.xml
DMABUF_XML    := $(PROTOCOL_DIR)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml

GENERATED     := xdg-shell-server-protocol.h xdg-shell-client-protocol.h xdg-shell-protocol.c \
                 fractional-scale-v1-server-protocol.h fractional-scale-v1-protocol.c \
                 xdg-decoration-server-protocol.h xdg-decoration-protocol.c \
                 text-input-v3-server-protocol.h text-input-v3-client-protocol.h \
                 text-input-v3-protocol.c \
                 linux-dmabuf-unstable-v1-server-protocol.h linux-dmabuf-unstable-v1-protocol.c
SOURCES       := main.c blob.c dmabuf.c hostlink.c xdg-shell-protocol.c fractional-scale-v1-protocol.c \
                 xdg-decoration-protocol.c text-input-v3-protocol.c \
                 linux-dmabuf-unstable-v1-protocol.c
BINARY        := nativepipe-wayland
CLIENT        := shmclient
VKPRESENT     := vkpresent
ALIGN_BLOB    := ../align-host-blob.so

.PHONY: all clean install

all: $(BINARY) $(CLIENT) $(ALIGN_BLOB)

$(ALIGN_BLOB): ../align-host-blob.c
	$(CC) -O2 -shared -fPIC -o $@ $< -ldl

# xdg-shell is not in libwayland; the scanner turns the XML into the server-side
# marshalling code.
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

$(BINARY): $(GENERATED) $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDLIBS)

# A test client, built against wayland-client rather than the server library.
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

clean:
	rm -f $(BINARY) $(CLIENT) $(VKPRESENT) $(ALIGN_BLOB) $(GENERATED)
