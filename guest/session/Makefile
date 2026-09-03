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
ALIGN   := dist/nativepipe-align-blob-$(ARCH)-$(LIBC).so
LAYER   := dist/nativepipe-vulkan-layer-$(ARCH)-$(LIBC).so

.PHONY: all dist-target check-arch check-deps clean

all: dist-target
dist-target: check-arch check-deps $(SESSION) $(ALIGN) $(LAYER)

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

$(ALIGN): $(ALIGN_SOURCE) | dist
	$(CC) $(BASE_CFLAGS) $(DRM_CFLAGS) -shared -fPIC -fvisibility=hidden \
		-o $@ $< -ldl
	$(STRIP) $@

$(LAYER): $(LAYER_SOURCE) | dist
	$(CC) $(BASE_CFLAGS) $(VULKAN_CFLAGS) -shared -fPIC -fvisibility=hidden \
		-o $@ $< -pthread
	$(STRIP) $@

clean:
	rm -rf dist
