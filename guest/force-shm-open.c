#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Test-environment shim for GTK on the persistent Alpine development image.
 * Its memfd can be opened but cannot be resized, while the POSIX shm tmpfs is
 * normal. Reporting ENOSYS makes GTK use its built-in shm_open fallback.
 */
int memfd_create(const char *name, unsigned int flags) {
	(void)name;
	(void)flags;
	errno = ENOSYS;
	return -1;
}

int ftruncate(int fd, off_t length) {
	static int (*next_ftruncate)(int, off_t);
	if (!next_ftruncate)
		next_ftruncate = dlsym(RTLD_NEXT, "ftruncate");
	fprintf(stderr, "[shm-probe] ftruncate fd=%d length=%lld\n",
	        fd, (long long)length);
	return next_ftruncate(fd, length);
}

void *cairo_image_surface_create_for_data(
	unsigned char *data, int format, int width, int height, int stride
) {
	static void *(*next_create)(unsigned char *, int, int, int, int);
	if (!next_create)
		next_create = dlsym(RTLD_NEXT, "cairo_image_surface_create_for_data");
	fprintf(stderr, "[shm-probe] cairo width=%d height=%d stride=%d\n",
	        width, height, stride);
	return next_create(data, format, width, height, stride);
}

int cairo_format_stride_for_width(int format, int width) {
	static int (*next_stride)(int, int);
	if (!next_stride)
		next_stride = dlsym(RTLD_NEXT, "cairo_format_stride_for_width");
	int stride = next_stride(format, width);
	fprintf(stderr, "[shm-probe] stride width=%d format=%d -> %d\n",
	        width, format, stride);
	return stride;
}
