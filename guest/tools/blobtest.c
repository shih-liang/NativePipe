// blobtest — proves the aperture carries real memory, in both directions.
//
// Everything up to now has been verified on one side or the other: the host can
// hand pages to Metal and CoreAnimation, and the guest's driver binds to the
// device and finds the host-visible window. This closes the loop:
//
//   host   fills a fresh resource with a known pattern
//   guest  creates the blob, maps it, and reads that pattern back
//   guest  writes its own pattern
//   host   reads the guest's bytes out of the same IOSurface
//
// If both patterns survive the trip, `RESOURCE_CREATE_BLOB` ->
// `RESOURCE_MAP_BLOB` -> `VZVirtioSharedMemoryRegion.mapMemory` is a single
// piece of memory rather than two that happen to agree.
//
//   apk add build-base linux-headers libdrm-dev
//   cc -O2 -o blobtest blobtest.c && ./blobtest

#define _GNU_SOURCE

#include <drm/drm.h>
#include <drm/virtgpu_drm.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define RENDER_NODE "/dev/dri/renderD128"

// Must match VirtioGPUDevice.swift.
#define HOST_PATTERN  0xA5A5A5A5u
#define GUEST_PATTERN 0x5A5A5A5Au

static const size_t BLOB_SIZE = 64 * 1024;  // page-aligned, as mapMemory requires

static void dump(const char *label, const uint32_t *words) {
	printf("%s %08x %08x %08x %08x\n", label, words[0], words[1], words[2], words[3]);
}

int main(void) {
	int fd = open(RENDER_NODE, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open " RENDER_NODE);
		return 1;
	}

	// Ask what the device supports before assuming anything.
	struct drm_virtgpu_getparam param;
	uint64_t value = 0;
	memset(&param, 0, sizeof(param));
	param.param = VIRTGPU_PARAM_RESOURCE_BLOB;
	param.value = (uint64_t)(uintptr_t)&value;
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &param) < 0) {
		perror("GETPARAM RESOURCE_BLOB");
	} else {
		printf("param RESOURCE_BLOB      = %llu\n", (unsigned long long)value);
	}

	value = 0;
	memset(&param, 0, sizeof(param));
	param.param = VIRTGPU_PARAM_HOST_VISIBLE;
	param.value = (uint64_t)(uintptr_t)&value;
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &param) < 0) {
		perror("GETPARAM HOST_VISIBLE");
	} else {
		printf("param HOST_VISIBLE       = %llu\n", (unsigned long long)value);
	}

	value = 0;
	memset(&param, 0, sizeof(param));
	param.param = VIRTGPU_PARAM_3D_FEATURES;
	param.value = (uint64_t)(uintptr_t)&value;
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &param) == 0) {
		printf("param 3D_FEATURES        = %llu\n", (unsigned long long)value);
	}

	value = 0;
	memset(&param, 0, sizeof(param));
	param.param = VIRTGPU_PARAM_CONTEXT_INIT;
	param.value = (uint64_t)(uintptr_t)&value;
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &param) == 0) {
		printf("param CONTEXT_INIT       = %llu\n", (unsigned long long)value);
	}

	// A host-memory blob needs a context to belong to. The capset selects the
	// renderer; venus is 4.
	struct drm_virtgpu_context_set_param ctx_params[1];
	struct drm_virtgpu_context_init ctx_init;
	memset(ctx_params, 0, sizeof(ctx_params));
	memset(&ctx_init, 0, sizeof(ctx_init));
	ctx_params[0].param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID;
	ctx_params[0].value = 4;  // VIRTIO_GPU_CAPSET_VENUS
	ctx_init.num_params = 1;
	ctx_init.ctx_set_params = (uint64_t)(uintptr_t)ctx_params;
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ctx_init) < 0) {
		printf("CONTEXT_INIT failed: %s\n", strerror(errno));
	} else {
		printf("context created with capset 4 (venus)\n");
	}

	// The resource itself: host memory, mappable into the guest.
	struct drm_virtgpu_resource_create_blob create;
	memset(&create, 0, sizeof(create));
	create.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
	create.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
	create.size = BLOB_SIZE;
	create.blob_id = 0;
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &create) < 0) {
		printf("RESOURCE_CREATE_BLOB failed: %s\n", strerror(errno));
		close(fd);
		return 2;
	}
	printf("blob created: bo_handle=%u res_handle=%u size=%zu\n",
	       create.bo_handle, create.res_handle, BLOB_SIZE);

	// Turn the buffer object into an mmap offset.
	struct drm_virtgpu_map map;
	memset(&map, 0, sizeof(map));
	map.handle = create.bo_handle;
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &map) < 0) {
		printf("VIRTGPU_MAP failed: %s\n", strerror(errno));
		close(fd);
		return 3;
	}

	void *pixels = mmap(NULL, BLOB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	                    (off_t)map.offset);
	if (pixels == MAP_FAILED) {
		printf("mmap failed: %s\n", strerror(errno));
		close(fd);
		return 4;
	}
	printf("mapped at %p\n", pixels);

	uint32_t *words = pixels;

	// Direction 1: does the host's pattern arrive here?
	dump("guest sees          ", words);
	int host_visible_ok = (words[0] == HOST_PATTERN);
	printf("host -> guest       %s (expected %08x)\n",
	       host_visible_ok ? "OK" : "MISMATCH", HOST_PATTERN);

	// Direction 2: leave something for the host to find.
	for (size_t i = 0; i < BLOB_SIZE / sizeof(uint32_t); i++) {
		words[i] = GUEST_PATTERN;
	}
	words[1] = 0xDEADBEEFu;
	words[2] = create.res_handle;
	__sync_synchronize();
	dump("guest wrote         ", words);

	printf("guest -> host       written; check the host log for resource %u\n",
	       create.res_handle);

	// Unmapping is the host's cue to read the bytes back.
	munmap(pixels, BLOB_SIZE);

	struct drm_gem_close gem_close;
	memset(&gem_close, 0, sizeof(gem_close));
	gem_close.handle = create.bo_handle;
	ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gem_close);

	close(fd);
	return host_visible_ok ? 0 : 5;
}
