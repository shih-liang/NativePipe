#define _GNU_SOURCE

#include "blob.h"

#include <drm/drm.h>
#include <drm/virtgpu_drm.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CAPSET_VENUS 4
#define FIRST_RENDER_NODE 128
#define LAST_RENDER_NODE 191

static size_t aperture_align(size_t size) {
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	size_t alignment = page > NP_APERTURE_ALIGNMENT ? page : NP_APERTURE_ALIGNMENT;
	return (size + alignment - 1) & ~(alignment - 1);
}

int np_blob_open(void) {
	int nodes_seen = 0;
	int last_error = ENOENT;

	// Device numbering is not an ABI.  When VZ's scanout GPU and NativePipe's
	// render-only custom GPU are both enabled, the former is normally renderD128
	// and the Venus device becomes renderD129.  Probe by capability instead of
	// silently binding the compositor to whichever driver registered first.
	for (int minor = FIRST_RENDER_NODE; minor <= LAST_RENDER_NODE; minor++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", minor);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			if (errno != ENOENT) last_error = errno;
			continue;
		}
		nodes_seen++;

		// Host-memory blobs live in a context, and capset 4 selects Venus.
		struct drm_virtgpu_context_set_param params[1];
		struct drm_virtgpu_context_init init;
		memset(params, 0, sizeof(params));
		memset(&init, 0, sizeof(init));
		params[0].param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID;
		params[0].value = CAPSET_VENUS;
		init.num_params = 1;
		init.ctx_set_params = (uint64_t)(uintptr_t)params;
		if (ioctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &init) == 0) {
			fprintf(stderr, "[wayland] Venus render node %s\n", path);
			return fd;
		}
		last_error = errno;
		close(fd);
	}

	errno = last_error;
	if (nodes_seen == 0)
		fprintf(stderr, "[wayland] no DRM render nodes: %s\n", strerror(errno));
	else
		fprintf(stderr, "[wayland] no render node accepts Venus capset 4: %s\n",
		        strerror(errno));
	return -1;
}

void np_blob_close(int fd) {
	if (fd >= 0) close(fd);
}

bool np_blob_create(int fd, size_t size, uint32_t width, uint32_t height,
                    uint32_t stride, struct np_blob *out) {
	memset(out, 0, sizeof(*out));
	size = aperture_align(size);

	struct drm_virtgpu_resource_create_blob create;
	memset(&create, 0, sizeof(create));
	create.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
	create.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
	create.size = size;
	create.blob_id = np_blob_pack_geometry(width, height, stride);
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &create) < 0) {
		fprintf(stderr, "[wayland] RESOURCE_CREATE_BLOB(%zu): %s\n", size, strerror(errno));
		return false;
	}

	struct drm_virtgpu_map map;
	memset(&map, 0, sizeof(map));
	map.handle = create.bo_handle;
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &map) < 0) {
		fprintf(stderr, "[wayland] VIRTGPU_MAP: %s\n", strerror(errno));
		goto fail;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map.offset);
	if (data == MAP_FAILED) {
		// EINVAL here usually means the host refused RESOURCE_MAP_BLOB, which
		// leaves the kernel's mapping state unusable even though the ioctls
		// above succeeded.
		fprintf(stderr, "[wayland] mmap size=%zu offset=0x%llx res=%u: %s\n",
		        size, (unsigned long long)map.offset, create.res_handle, strerror(errno));
		goto fail;
	}

	out->bo_handle = create.bo_handle;
	out->resource_id = create.res_handle;
	out->size = size;
	out->data = data;
	return true;

fail: {
	struct drm_gem_close gem_close;
	memset(&gem_close, 0, sizeof(gem_close));
	gem_close.handle = create.bo_handle;
	ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
	return false;
}
}

void np_blob_destroy(int fd, struct np_blob *blob) {
	if (blob->data) {
		munmap(blob->data, blob->size);
		blob->data = NULL;
	}
	if (blob->bo_handle) {
		struct drm_gem_close gem_close;
		memset(&gem_close, 0, sizeof(gem_close));
		gem_close.handle = blob->bo_handle;
		ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
		blob->bo_handle = 0;
	}
	blob->resource_id = 0;
	blob->size = 0;
}
