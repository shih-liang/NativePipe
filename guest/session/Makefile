# Build one native guest-runtime target. CI supplies an aarch64 or x86_64
# runner inside the matching glibc/musl container; no unpinned cross toolchain
# is downloaded by this repository.

CC      ?= cc
STRIP   ?= strip
ARCH    ?= $(shell uname -m | sed 's/^arm64$$/aarch64/')
LIBC    ?= $(shell (ldd --version 2>&1 || true) | grep -qi musl && echo musl || echo gnu)

COMMON_SOURCE := ../common/np.c
ALIGN_SOURCE  := ../align-host-blob.c
LAYER_SOURCE  := ../vulkan-blob-alignment-layer.c

BASE_CFLAGS  := -O2 -Wall -Wextra -Werror -fno-ident
SESSION_LINK :=
ifeq ($(LIBC),musl)
SESSION_LINK := -static
endif

VULKAN_CFLAGS := $(shell pkg-config --cflags vulkan 2>/dev/null)
DRM_CFLAGS    := $(shell pkg-config --cflags libdrm 2>/dev/null)

SESSION := dist/nativepipe-session-$(ARCH)-$(LIBC)
OPEN    := dist/nativepipe-open-$(ARCH)-$(LIBC)
ALIGN   := dist/nativepipe-align-blob-$(ARCH)-$(LIBC).so
LAYER   := dist/nativepipe-vulkan-layer-$(ARCH)-$(LIBC).so

.PHONY: all dist-target check-arch check-deps test test-runtime test-np-open test-np-open-cli clean

all: dist-target
dist-target: check-arch check-deps $(SESSION) $(OPEN) $(ALIGN) $(LAYER)

check-arch:
	@actual="$$(uname -m | sed 's/^arm64$$/aarch64/')"; \
	[ "$$actual" = "$(ARCH)" ] || { \
		echo "error: native build runner is $$actual, requested $(ARCH)" >&2; exit 1; \
	}

check-deps:
	@pkg-config --exists libdrm vulkan || { \
		echo 'error: libdrm and Vulkan development headers are required' >&2; exit 1; \
	}

dist:
	mkdir -p $@

$(SESSION): nativepipe-session.c $(COMMON_SOURCE) | dist
	$(CC) $(BASE_CFLAGS) -I../common $(SESSION_LINK) -o $@ $^
	$(STRIP) $@

# np-open asks the Mac to open a URL or file. Static on musl like the session.
$(OPEN): np-open.c np-open-wire.h $(COMMON_SOURCE) | dist
	$(CC) $(BASE_CFLAGS) -I../common $(SESSION_LINK) -o $@ np-open.c $(COMMON_SOURCE)
	$(STRIP) $@

$(ALIGN): $(ALIGN_SOURCE) | dist
	$(CC) $(BASE_CFLAGS) $(DRM_CFLAGS) -shared -fPIC -fvisibility=hidden \
		-o $@ $< -ldl
	$(STRIP) $@

$(LAYER): $(LAYER_SOURCE) | dist
	$(CC) $(BASE_CFLAGS) $(VULKAN_CFLAGS) -shared -fPIC -fvisibility=hidden \
		-o $@ $< -pthread
	$(STRIP) $@

test-runtime: dist/test-runtime
	dist/test-runtime

# Wire format and argument handling of np-open; needs no guest or vsock.
test-np-open: dist/test-np-open
	dist/test-np-open

dist/test-np-open: test-np-open.c np-open-wire.h ../common/np.h | dist
	$(CC) $(BASE_CFLAGS) -I../common -o $@ test-np-open.c

test-np-open-cli: dist/test-np-open-cli
	dist/test-np-open-cli

dist/test-np-open-cli: test-np-open-cli.c np-open.c np-open-wire.h ../common/np.h | dist
	$(CC) $(BASE_CFLAGS) -I../common -o $@ test-np-open-cli.c

dist/test-runtime: test-runtime.c nativepipe-session.c ../common/np.h $(COMMON_SOURCE) | dist
	$(CC) $(BASE_CFLAGS) -I../common -o $@ test-runtime.c $(COMMON_SOURCE)

test: $(ALIGN) test-runtime test-np-open test-np-open-cli
	$(CC) $(BASE_CFLAGS) -shared -fPIC -DTEST_IOCTL_BACKEND \
		-o dist/test-blob-backend.so test-blob-interposition.c
	$(CC) $(BASE_CFLAGS) -o dist/test-blob-client test-blob-interposition.c -ldl
	LD_PRELOAD='$(abspath $(ALIGN)):$(abspath dist/test-blob-backend.so)' dist/test-blob-client
	$(CC) $(BASE_CFLAGS) $(DRM_CFLAGS) -DNATIVEPIPE_ALIGNMENT_SELF_TEST \
		-o dist/test-blob-alignment $(ALIGN_SOURCE) -ldl
	dist/test-blob-alignment

clean:
	rm -rf dist
