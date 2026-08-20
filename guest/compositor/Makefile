# Built inside the guest / remote Linux host.
#
# VM compositor uses a single Vulkan-backed surface allocation.
# CPU writes and GPU rendering share the same VkImage/VkDeviceMemory.
#
# The previous np_blob_open()/RESOURCE_CREATE_BLOB mmap path is being removed.

PKGS          := wayland-server libcjson xkbcommon
CLIENT_PKGS   := wayland-client
CFLAGS        += -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(shell pkg-config --cflags $(PKGS))
LDLIBS        += $(shell pkg-config --libs $(PKGS))

PROTOCOL_DIR  := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML := $(PROTOCOL_DIR)/stable/xdg-shell/xdg-shell.xml

GENERATED := xdg-shell-server-protocol.h xdg-shell-client-protocol.h xdg-shell-protocol.c

SHARED_SOURCES := compositor.c hostlink.c xdg-shell-protocol.c

# VM compositor: Vulkan only. No manual virtio-gpu context/blob allocation.
VM_SOURCES := vmpipe_main.c vk_surface_buffer.c

BINARY := vmpipe-wayland

.PHONY: all clean

all: $(BINARY)

xdg-shell-server-protocol.h:
	wayland-scanner server-header $(XDG_SHELL_XML) $@

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(XDG_SHELL_XML) $@

xdg-shell-protocol.c:
	wayland-scanner private-code $(XDG_SHELL_XML) $@

$(BINARY): $(GENERATED) $(SHARED_SOURCES) $(VM_SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SHARED_SOURCES) $(VM_SOURCES) $(LDLIBS)

clean:
	rm -f $(BINARY) $(GENERATED)
