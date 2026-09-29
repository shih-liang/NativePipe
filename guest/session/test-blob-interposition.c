#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#if defined(__GLIBC__)
typedef unsigned long request_t;
#else
typedef int request_t;
#endif

#ifdef TEST_IOCTL_BACKEND
/* A downstream driver stand-in: the preload library must intercept first. */
int ioctl(int fd, request_t request, ...)
{
    (void)fd;
    (void)request;
    return 0;
}
#else
struct blob_prefix {
    uint32_t mem, flags, handle, resource;
    uint64_t size;
    uint32_t pad, command_size;
    uint64_t command, id;
};

int main(void)
{
    assert(sizeof(struct blob_prefix) == 48);
    int (*drm_ioctl)(int, unsigned long, void *) = dlsym(RTLD_DEFAULT, "drmIoctl");
    assert(drm_ioctl);
    for (int via_drm = 0; via_drm < 2; ++via_drm) {
        for (unsigned size = 48; size <= 64; size += 8) {
            struct { struct blob_prefix blob; uint64_t tail[2]; } value = {
                .blob = {.mem = 2, .flags = 1, .size = 131268},
                .tail = {UINT64_MAX, UINT64_MAX},
            };
            unsigned long request = _IOC(_IOC_READ | _IOC_WRITE, 'd', 0x4a, size);
            int result = via_drm ? drm_ioctl(99, request, &value) :
                ioctl(99, (request_t)request, &value);
            assert(result == 0 && value.blob.size == 147456);
            assert(value.tail[0] == UINT64_MAX && value.tail[1] == UINT64_MAX);
        }
    }
    const unsigned long unrelated[] = {
        _IOC(_IOC_READ | _IOC_WRITE, 'd', 0x4a, 40),
        _IOC(_IOC_READ | _IOC_WRITE, 'x', 0x4a, 48),
        _IOC(_IOC_READ | _IOC_WRITE, 'd', 0x49, 48),
        _IOC(_IOC_WRITE, 'd', 0x4a, 48),
    };
    for (size_t i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); ++i) {
        struct blob_prefix value = {.mem = 2, .flags = 1, .size = 4097};
        assert(ioctl(99, (request_t)unrelated[i], &value) == 0);
        assert(value.size == 4097);
    }
    /* Hidden symbols broke preload without failing a normal link or unit test. */
    for (size_t i = 0; i < 3; ++i) {
        const char *names[] = {"mmap", "munmap", "ioctl"};
        Dl_info info;
        assert(dladdr(dlsym(RTLD_DEFAULT, names[i]), &info) != 0);
        assert(strstr(info.dli_fname, "nativepipe-align-blob-"));
    }
    puts("Blob preload: exported hooks, legacy and extended UAPI, unrelated requests OK");
    return 0;
}
#endif
