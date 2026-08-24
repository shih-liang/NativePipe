#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/virtgpu_drm.h>

/* Low-kernel workaround for every guest-mappable Venus blob.  The implicit
 * Vulkan layer grows application VkDeviceMemory first; this ioctl boundary
 * also catches host-visible allocations made internally by the Venus ICD,
 * which never pass through an application layer.  The host renderer rejects
 * the map unless the exported MTLBuffer is at least this aligned size. */
#define HOST_PAGE ((uint64_t)16384)

static int trace_enabled(void)
{
	const char *value = getenv("NATIVEPIPE_GPU_TRACE");
	return value && value[0] && strcmp(value, "0") != 0;
}

static int align_create_blob(struct drm_virtgpu_resource_create_blob *create)
{
	if (!create || create->size == 0)
		return 0;
	const int mappable =
		(create->blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE) != 0;
	if (trace_enabled() && mappable) {
		fprintf(stderr,
		        "[align-blob] CREATE_BLOB id=%llu mem=%u flags=0x%x size=%llu\n",
		        (unsigned long long)create->blob_id,
		        create->blob_mem, create->blob_flags,
		        (unsigned long long)create->size);
	}
	if (!mappable)
		return 0;
	if (create->size > UINT64_MAX - (HOST_PAGE - 1)) {
		errno = EOVERFLOW;
		return -1;
	}
	uint64_t aligned = (create->size + HOST_PAGE - 1) & ~(HOST_PAGE - 1);
	if (aligned == create->size) return 0;
	if (trace_enabled()) {
		fprintf(stderr, "[align-blob] CREATE_BLOB size %llu -> %llu\n",
		        (unsigned long long)create->size, (unsigned long long)aligned);
	}
	create->size = aligned;
	return 0;
}

__attribute__((constructor))
static void align_blob_loaded(void)
{
	if (!trace_enabled()) return;
	const char msg[] = "[align-blob] loaded\n";
	(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
}

/* musl: ioctl(int, int, ...). glibc: ioctl(int, unsigned long, ...). */
#if defined(__GLIBC__)
typedef unsigned long np_ioctl_request_t;
#else
typedef int np_ioctl_request_t;
#endif

static int is_create_blob_ioctl(np_ioctl_request_t request)
{
#if defined(__GLIBC__)
	return request == DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB;
#else
	/* musl declares request as int; _IOWR constants are unsigned long. */
	return (unsigned int)request ==
	       (unsigned int)DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB;
#endif
}

int ioctl(int fd, np_ioctl_request_t request, ...)
{
	static int (*next_ioctl)(int, np_ioctl_request_t, ...);
	if (!next_ioctl) {
		next_ioctl = (int (*)(int, np_ioctl_request_t, ...))dlsym(RTLD_NEXT, "ioctl");
	}

	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	if (is_create_blob_ioctl(request) && arg) {
		if (align_create_blob(arg) < 0) return -1;
	}
	if (!next_ioctl) {
		errno = ENOSYS;
		return -1;
	}
	return next_ioctl(fd, request, arg);
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
	static int (*next_drm)(int, unsigned long, void *);
	if (!next_drm) {
		next_drm = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "drmIoctl");
	}
	if (request == DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB && arg) {
		if (align_create_blob(arg) < 0) return -1;
	}
	if (next_drm) return next_drm(fd, request, arg);
	static int (*next_ioctl)(int, np_ioctl_request_t, ...);
	if (!next_ioctl) {
		next_ioctl = (int (*)(int, np_ioctl_request_t, ...))dlsym(RTLD_NEXT, "ioctl");
	}
	if (!next_ioctl) {
		errno = ENOSYS;
		return -1;
	}
	return next_ioctl(fd, (np_ioctl_request_t)request, arg);
}

#ifdef NATIVEPIPE_ALIGNMENT_SELF_TEST
#include <assert.h>
int main(void)
{
	struct drm_virtgpu_resource_create_blob ring = {
		.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE,
		.size = 4097,
		.blob_id = 0,
	};
	assert(align_create_blob(&ring) == 0 && ring.size == HOST_PAGE);
	struct drm_virtgpu_resource_create_blob memory = ring;
	memory.blob_id = 7;
	memory.size = 4097;
	assert(align_create_blob(&memory) == 0 && memory.size == HOST_PAGE);
	struct drm_virtgpu_resource_create_blob private_memory = memory;
	private_memory.blob_flags = 0;
	private_memory.size = 4097;
	assert(align_create_blob(&private_memory) == 0 &&
	       private_memory.size == 4097);
	assert(is_create_blob_ioctl(
	       (np_ioctl_request_t)DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB));
	return 0;
}
#endif
