#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/virtgpu_drm.h>

/*
 * Mesa Venus sizes blobs to the guest page (4 KiB). The host aperture is
 * mapped with VZ mapMemory, which requires the host page (16 KiB). The
 * guest drm_mm hands out offsets from CREATE size, so an unaligned CREATE
 * puts the next blob at an offset the host cannot map.
 *
 * Every guest-mappable blob consumes space in the same drm_mm aperture.  A
 * single 4 KiB VkDeviceMemory blob therefore misaligns all following rings,
 * even when every ring itself is rounded.  Round every USE_MAPPABLE blob; the
 * host renderer applies the same rule before allocating host-visible Vulkan
 * memory, so the vkr allocation and RESOURCE_CREATE_BLOB signatures agree.
 * DEVICE_LOCAL / Metal-heap blobs are not mappable and remain untouched.
 */
#define HOST_PAGE ((uint64_t)16384)

static int trace_enabled(void)
{
	const char *value = getenv("NATIVEPIPE_GPU_TRACE");
	return value && value[0] && strcmp(value, "0") != 0;
}

static void align_create_blob(struct drm_virtgpu_resource_create_blob *create)
{
	if (!create || create->size == 0) return;
	if (trace_enabled()) {
		fprintf(stderr,
		        "[align-blob] CREATE_BLOB mem=%u flags=0x%x blob_id=%llu size=%llu%s\n",
		        create->blob_mem, create->blob_flags,
		        (unsigned long long)create->blob_id,
		        (unsigned long long)create->size,
		        create->blob_id == 0 ? " ring" : "");
	}
	if (!(create->blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE)) return;
	uint64_t aligned = (create->size + HOST_PAGE - 1) & ~(HOST_PAGE - 1);
	if (aligned == create->size) return;
	if (trace_enabled()) {
		fprintf(stderr, "[align-blob] CREATE_BLOB size %llu -> %llu\n",
		        (unsigned long long)create->size, (unsigned long long)aligned);
	}
	create->size = aligned;
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

	if ((unsigned long)request == DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB && arg) {
		align_create_blob(arg);
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
		align_create_blob(arg);
	}
	if (next_drm) return next_drm(fd, request, arg);
	static int (*next_ioctl)(int, np_ioctl_request_t, ...);
	if (!next_ioctl) {
		next_ioctl = (int (*)(int, np_ioctl_request_t, ...))dlsym(RTLD_NEXT, "ioctl");
	}
	return next_ioctl(fd, (np_ioctl_request_t)request, arg);
}
