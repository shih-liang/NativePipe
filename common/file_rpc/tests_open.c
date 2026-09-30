#define _GNU_SOURCE
#undef NDEBUG
#include "np_file_rpc.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Build with an injected openat2 ENOSYS, on native Linux and Rosetta alike. */
int main(void) {
    char temporary[] = "/tmp/nativepipe-open-test.XXXXXX";
    assert(mkdtemp(temporary));
    int root = open("/", O_PATH | O_DIRECTORY), local = open(temporary, O_RDONLY | O_DIRECTORY);
    assert(root >= 0 && local >= 0);
    char path[4096], target[4096];
    snprintf(path, sizeof(path), "%s/file", temporary);
    int fd = np_file_open(root, path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, "content", 7) == 7); close(fd);
    assert(np_file_open(root, path, O_CREAT | O_EXCL | O_WRONLY, 0600) < 0 && errno == EEXIST);
    assert(symlinkat("file", local, "relative") == 0);
    assert(symlinkat(path, local, "absolute") == 0);
    assert(mkdirat(local, "sub", 0700) == 0);
    assert(symlinkat("../relative", local, "sub/parent") == 0);
    assert(symlinkat("sub", local, "directory") == 0);
    const char *names[] = {"file", "relative", "absolute", "sub/parent", "directory/parent", "sub/../file"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", temporary, names[i]);
        fd = np_file_open(root, path, O_RDONLY, 0); assert(fd >= 0);
        char bytes[8] = {0}; assert(read(fd, bytes, sizeof(bytes)) == 7 && !strcmp(bytes, "content")); close(fd);
    }
    snprintf(path, sizeof(path), "%s/relative", temporary);
    assert(np_file_open(root, path, O_RDONLY | O_NOFOLLOW, 0) < 0 && errno == ELOOP);
    fd = np_file_open(root, path, O_PATH | O_NOFOLLOW, 0); assert(fd >= 0);
    struct stat st; assert(fstat(fd, &st) == 0 && S_ISLNK(st.st_mode)); close(fd);
    assert(np_file_open(root, path, O_CREAT | O_EXCL | O_WRONLY, 0600) < 0 && errno == EEXIST);
    snprintf(path, sizeof(path), "%s/relative/", temporary);
    assert(np_file_open(root, path, O_RDONLY, 0) < 0 && errno == ENOTDIR);
    snprintf(path, sizeof(path), "%s/directory/", temporary);
    fd = np_file_open(root, path, O_RDONLY, 0); assert(fd >= 0);
    assert(fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)); close(fd);
    assert(symlinkat("loop", local, "loop") == 0);
    snprintf(path, sizeof(path), "%s/loop", temporary);
    assert(np_file_open(root, path, O_RDONLY, 0) < 0 && errno == ELOOP);
    /* Never weaken virtual-root confinement when openat2 is unavailable. */
    assert(np_file_open(local, "/file", O_RDONLY, 0) < 0 && errno == ENOSYS);
    assert(np_file_open(local, "/absolute", O_RDONLY, 0) < 0 && errno == ENOSYS);
    snprintf(path, sizeof(path), "/proc/%d/fd/%d", getpid(), local);
    assert(np_file_open(root, path, O_RDONLY, 0) < 0 && errno == ELOOP);
    /* A normal link to a procfs magic link is equally forbidden. */
    assert(symlinkat(path, local, "magic") == 0);
    snprintf(path, sizeof(path), "%s/magic", temporary);
    assert(np_file_open(root, path, O_RDONLY, 0) < 0 && errno == ELOOP);
    fd = np_file_open(root, "/proc/meminfo", O_RDONLY, 0); assert(fd >= 0);
    assert(read(fd, target, sizeof(target)) > 0); close(fd);
    if (geteuid() != 0) {
        assert(fchmodat(local, "file", 0, 0) == 0);
        snprintf(path, sizeof(path), "%s/absolute", temporary);
        assert(np_file_open(root, path, O_RDONLY, 0) < 0 && errno == EACCES);
        assert(fchmodat(local, "file", 0600, 0) == 0);
    }
    assert(unlinkat(local, "sub/parent", 0) == 0);
    const char *cleanup[] = {"file", "relative", "absolute", "directory", "loop", "magic"};
    for (unsigned i = 0; i < sizeof(cleanup) / sizeof(cleanup[0]); i++) assert(unlinkat(local, cleanup[i], 0) == 0);
    assert(unlinkat(local, "sub", AT_REMOVEDIR) == 0);
    close(local); close(root); assert(rmdir(temporary) == 0);
    puts("file open ENOSYS: reads, writes, symlinks, permissions, procfs and confined-root refusal PASS");
}
