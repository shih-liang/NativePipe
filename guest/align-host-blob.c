#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/virtgpu_drm.h>

/*
 * Mesa Venus sizes blobs to the guest page (4 KiB). The host aperture is
 * mapped with VZ mapMemory, which requires the host page (16 KiB). The
 * guest drm_mm hands out offsets from CREATE size, so an unaligned CREATE
 * puts the next blob at an offset the host cannot map.
 *
 * nativepipe-wayland already rounds its own blobs. This interposer does
 * the same for every other process (vulkaninfo, vkcube, GTK vulkan).
 */
#define HOST_PAGE ((uint64_t)16384)

static void align_create_blob(struct drm_virtgpu_resource_create_blob *create)
{
	if (!create || create->size == 0) return;
	uint64_t aligned = (create->size + HOST_PAGE - 1) & ~(HOST_PAGE - 1);
	if (aligned == create->size) return;
	fprintf(stderr, "[align-blob] CREATE_BLOB size %llu -> %llu\n",
	        (unsigned long long)create->size, (unsigned long long)aligned);
	create->size = aligned;
}

__attribute__((constructor))
static void align_blob_loaded(void)
{
	const char msg[] = "[align-blob] loaded\n";
	(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
}

/* musl: ioctl(int, int, ...). glibc: ioctl(int, unsigned long, ...). */
int ioctl(int fd, int request, ...)
{
	static int (*next_ioctl)(int, int, ...);
	if (!next_ioctl) {
		next_ioctl = (int (*)(int, int, ...))dlsym(RTLD_NEXT, "ioctl");
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
	static int (*next_ioctl)(int, int, ...);
	if (!next_ioctl) {
		next_ioctl = (int (*)(int, int, ...))dlsym(RTLD_NEXT, "ioctl");
	}
	return next_ioctl(fd, (int)request, arg);
}
