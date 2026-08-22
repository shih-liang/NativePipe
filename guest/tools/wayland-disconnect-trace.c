#define _GNU_SOURCE

/*
 * Diagnostic LD_PRELOAD helper for finding which side closes a Wayland
 * connection first. It is intentionally dormant unless NP_DISCONNECT_TRACE
 * names an output file, and logs only EOF/errors plus close/shutdown rather
 * than every byte transferred.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int trace_fd = -1;
static __thread bool inside_trace;

static long trace_tid(void)
{
    return syscall(SYS_gettid);
}

static void trace_write(const char *operation, int fd, ssize_t result,
                        int error, void *caller)
{
    if (trace_fd < 0 || inside_trace)
        return;
    inside_trace = true;

    struct sockaddr_un local = {0};
    struct sockaddr_un peer = {0};
    socklen_t local_len = sizeof(local);
    socklen_t peer_len = sizeof(peer);
    bool local_ok = getsockname(fd, (struct sockaddr *)&local, &local_len) == 0 &&
                    local.sun_family == AF_UNIX;
    bool peer_ok = getpeername(fd, (struct sockaddr *)&peer, &peer_len) == 0 &&
                   peer.sun_family == AF_UNIX;
    const char *local_path = local_ok && local.sun_path[0] ? local.sun_path : "-";
    const char *peer_path = peer_ok && peer.sun_path[0] ? peer.sun_path : "-";
    if ((!local_ok && !peer_ok) ||
        (!strstr(local_path, "wayland-") && !strstr(peer_path, "wayland-"))) {
        inside_trace = false;
        return;
    }

    Dl_info symbol = {0};
    dladdr(caller, &symbol);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const char *role = getenv("NP_TRACE_ROLE");
    if (!role) role = "unknown";

    char line[1536];
    int count = snprintf(
        line, sizeof(line),
        "%lld.%09ld role=%s pid=%ld tid=%ld op=%s fd=%d result=%zd errno=%d(%s) "
        "local=%s peer=%s caller=%s+0x%lx object=%s\n",
        (long long)now.tv_sec, now.tv_nsec, role, (long)getpid(), trace_tid(),
        operation, fd, result, error, error ? strerror(error) : "ok",
        local_path, peer_path,
        symbol.dli_sname ? symbol.dli_sname : "?",
        symbol.dli_saddr ? (unsigned long)((char *)caller - (char *)symbol.dli_saddr) : 0,
        symbol.dli_fname ? symbol.dli_fname : "?");
    if (count > 0) {
        size_t length = (size_t)count < sizeof(line) ? (size_t)count : sizeof(line) - 1;
        (void)syscall(SYS_write, trace_fd, line, length);
    }
    inside_trace = false;
}

__attribute__((constructor))
static void trace_start(void)
{
    const char *path = getenv("NP_DISCONNECT_TRACE");
    if (!path || !path[0])
        return;
    trace_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
}

#define RESOLVE(name) \
    static __typeof__(name) *real_##name; \
    if (!real_##name) real_##name = dlsym(RTLD_NEXT, #name)

int close(int fd)
{
    RESOLVE(close);
    void *caller = __builtin_return_address(0);
    trace_write("close", fd, 0, 0, caller);
    return real_close(fd);
}

int shutdown(int fd, int how)
{
    RESOLVE(shutdown);
    void *caller = __builtin_return_address(0);
    int result = real_shutdown(fd, how);
    int saved = errno;
    trace_write("shutdown", fd, result, result < 0 ? saved : 0, caller);
    errno = saved;
    return result;
}

#define TRACE_IO(result_type, name, arguments, call_arguments) \
    result_type name arguments \
    { \
        RESOLVE(name); \
        void *caller = __builtin_return_address(0); \
        result_type result = real_##name call_arguments; \
        int saved = errno; \
        if (result <= 0) \
            trace_write(#name, fd, (ssize_t)result, result < 0 ? saved : 0, caller); \
        errno = saved; \
        return result; \
    }

TRACE_IO(ssize_t, sendmsg,
         (int fd, const struct msghdr *message, int flags),
         (fd, message, flags))
TRACE_IO(ssize_t, recvmsg,
         (int fd, struct msghdr *message, int flags),
         (fd, message, flags))
TRACE_IO(ssize_t, send,
         (int fd, const void *buffer, size_t length, int flags),
         (fd, buffer, length, flags))
TRACE_IO(ssize_t, recv,
         (int fd, void *buffer, size_t length, int flags),
         (fd, buffer, length, flags))
TRACE_IO(ssize_t, write,
         (int fd, const void *buffer, size_t length),
         (fd, buffer, length))
TRACE_IO(ssize_t, read,
         (int fd, void *buffer, size_t length),
         (fd, buffer, length))

