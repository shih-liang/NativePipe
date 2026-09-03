#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <drm/virtgpu_drm.h>

/* Low-kernel workaround for every guest-mappable Venus blob.  The implicit
 * Vulkan layer grows application VkDeviceMemory first; this ioctl boundary
 * also catches host-visible allocations made internally by the Venus ICD,
 * which never pass through an application layer.  The host renderer rejects
 * the map unless the exported MTLBuffer is at least this aligned size. */
#define HOST_PAGE ((uint64_t)16384)
#define TRACKED_MAPPINGS 256

struct aligned_mapping {
	void *address;
	size_t length;
};

static atomic_int mappable_blob_fd = -1;
static struct aligned_mapping aligned_mappings[TRACKED_MAPPINGS];
static atomic_flag mapping_lock = ATOMIC_FLAG_INIT;

static size_t align_host_page(size_t size)
{
	if (size > SIZE_MAX - (HOST_PAGE - 1))
		return 0;
	return (size + HOST_PAGE - 1) & ~(HOST_PAGE - 1);
}

static void lock_mappings(void)
{
	while (atomic_flag_test_and_set_explicit(&mapping_lock,
	                                        memory_order_acquire)) {}
}

static void unlock_mappings(void)
{
	atomic_flag_clear_explicit(&mapping_lock, memory_order_release);
}

static int remember_mapping(void *address, size_t length)
{
	int remembered = 0;
	lock_mappings();
	for (size_t i = 0; i < TRACKED_MAPPINGS; i++) {
		if (!aligned_mappings[i].address) {
			aligned_mappings[i].address = address;
			aligned_mappings[i].length = length;
			remembered = 1;
			break;
		}
	}
	unlock_mappings();
	return remembered;
}

static size_t forget_mapping(void *address)
{
	size_t length = 0;
	lock_mappings();
	for (size_t i = 0; i < TRACKED_MAPPINGS; i++) {
		if (aligned_mappings[i].address == address) {
			length = aligned_mappings[i].length;
			aligned_mappings[i].address = NULL;
			aligned_mappings[i].length = 0;
			break;
		}
	}
	unlock_mappings();
	return length;
}

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
	uint64_t aligned = align_host_page((size_t)create->size);
	if (!aligned) {
		errno = EOVERFLOW;
		return -1;
	}
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

static void record_mappable_blob_fd(int fd, const void *arg, int result)
{
	if (result != 0 || !arg ||
	    !(((const struct drm_virtgpu_resource_create_blob *)arg)->blob_flags &
	      VIRTGPU_BLOB_FLAG_USE_MAPPABLE))
		return;
	atomic_store_explicit(&mappable_blob_fd, fd, memory_order_release);
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
	int result = next_ioctl(fd, request, arg);
	if (is_create_blob_ioctl(request))
		record_mappable_blob_fd(fd, arg, result);
	return result;
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
	if (next_drm) {
		int result = next_drm(fd, request, arg);
		if (request == DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB)
			record_mappable_blob_fd(fd, arg, result);
		return result;
	}
	static int (*next_ioctl)(int, np_ioctl_request_t, ...);
	if (!next_ioctl) {
		next_ioctl = (int (*)(int, np_ioctl_request_t, ...))dlsym(RTLD_NEXT, "ioctl");
	}
	if (!next_ioctl) {
		errno = ENOSYS;
		return -1;
	}
	int result = next_ioctl(fd, (np_ioctl_request_t)request, arg);
	if (request == DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB)
		record_mappable_blob_fd(fd, arg, result);
	return result;
}

/* VZ maps the host pointer into its shared-memory aperture in host pages.
 * Linux before VIRTGPU_PARAM_BLOB_ALIGNMENT still asks mmap with the exact
 * Venus shmem payload length (for example 131268 bytes), even when CREATE_BLOB
 * was rounded above.  Extend only mappings on the fd that successfully created
 * a mappable virtio-gpu blob, and remember the real span for munmap. */
static void *map_aligned_blob(void *address, size_t length, int prot, int flags,
	                         int fd, off_t offset)
{
	size_t mapped_length = length;
	if (fd == atomic_load_explicit(&mappable_blob_fd, memory_order_acquire) &&
	    (flags & MAP_SHARED)) {
		mapped_length = align_host_page(length);
		if (!mapped_length) {
			errno = EOVERFLOW;
			return MAP_FAILED;
		}
	}
	void *result = (void *)syscall(SYS_mmap, address, mapped_length, prot,
	                              flags, fd, offset);
	if (result == MAP_FAILED || mapped_length == length)
		return result;
	if (!remember_mapping(result, mapped_length)) {
		(void)syscall(SYS_munmap, result, mapped_length);
		errno = ENOMEM;
		return MAP_FAILED;
	}
	if (trace_enabled()) {
		fprintf(stderr, "[align-blob] mmap size %zu -> %zu offset=%lld\n",
		        length, mapped_length, (long long)offset);
	}
	return result;
}

void *mmap(void *address, size_t length, int prot, int flags, int fd,
	  off_t offset)
{
	return map_aligned_blob(address, length, prot, flags, fd, offset);
}

#if defined(__GLIBC__)
void *mmap64(void *address, size_t length, int prot, int flags, int fd,
	    off64_t offset)
{
	return map_aligned_blob(address, length, prot, flags, fd, (off_t)offset);
}
#endif

int munmap(void *address, size_t length)
{
	size_t mapped_length = forget_mapping(address);
	return (int)syscall(SYS_munmap, address,
	                    mapped_length ? mapped_length : length);
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
	assert(align_host_page(131268) == 147456);
	assert(align_host_page(147456) == 147456);
	return 0;
}
#endif
