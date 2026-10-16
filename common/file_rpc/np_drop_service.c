#define _GNU_SOURCE
#include "np_drop_fs.h"

#ifdef __linux__
#include "np_file_rpc.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define WORKERS 4u
/* A WRITE carries max_write bytes after its headers; the kernel refuses reads
 * into a buffer that could not hold one. */
#define REQUEST_BUFFER (NP_DROP_MAX_WRITE + 4096u)
#define FUSE_SETLKW_OPCODE 33u

struct np_drop_service {
    struct np_drop_fs *fs;
    /* Remote: one session over the connection given at start; no listener. */
    int remote;
    int fuse, listener, wake[2];
    char path[256];
    pthread_t acceptor;
    struct np_file2_session *session;
};

/* guestd checks our credentials, mounts for this user and passes /dev/fuse. */
static int request_mount(char *path, size_t capacity) {
    int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) return -1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, NP_DROP_MOUNT_SOCKET);
    int fuse = -1, error = EPROTO;
    unsigned char reply[4 + 4 + 2 + 256];
    union { struct cmsghdr header; char space[CMSG_SPACE(sizeof(int))]; } control;
    struct iovec vector = {reply, sizeof(reply)};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1,
                             .msg_control = control.space, .msg_controllen = sizeof(control.space)};
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        write(socket_fd, "NPDM", 4) != 4) { error = errno; goto done; }
    ssize_t received = recvmsg(socket_fd, &message, MSG_CMSG_CLOEXEC);
    struct cmsghdr *header = received > 0 ? CMSG_FIRSTHDR(&message) : NULL;
    if (header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
        header->cmsg_len == CMSG_LEN(sizeof(int))) memcpy(&fuse, CMSG_DATA(header), sizeof(int));
    if (received < 10 || memcmp(reply, "NPDM", 4)) goto done;
    uint32_t status = np_file_u32(reply + 4);
    size_t length = (size_t)(reply[8] | reply[9] << 8);
    if (status) { error = (int)status; goto done; }
    if (fuse < 0 || (size_t)received != 10 + length || !length || length >= capacity || reply[10] != '/') goto done;
    memcpy(path, reply + 10, length); path[length] = 0;
    close(socket_fd);
    return fuse;
done:
    if (fuse >= 0) close(fuse);
    close(socket_fd);
    errno = error;
    return -1;
}

static int write_reply(void *context, const struct iovec *vector, int count) {
    int fuse = *(int *)context;
    ssize_t written = writev(fuse, vector, count);
    /* ENOENT: the request was interrupted and no longer awaits an answer. */
    return written < 0 && errno != ENOENT ? -1 : 0;
}

struct waiting { struct np_drop_service *service; size_t length; unsigned char request[]; };

/* A blocking lock may wait indefinitely; it must not occupy a worker. */
static void *serve_waiting(void *argument) {
    struct waiting *waiting = argument;
    if (np_drop_fs_handle(waiting->service->fs, waiting->request, waiting->length, write_reply,
                          &waiting->service->fuse) < 0)
        fprintf(stderr, "[drops] malformed FUSE request\n");
    free(waiting);
    return NULL;
}

static void *serve_fuse(void *argument) {
    struct np_drop_service *service = argument;
    unsigned char *buffer = malloc(REQUEST_BUFFER);
    while (buffer) {
        ssize_t length = read(service->fuse, buffer, REQUEST_BUFFER);
        if (length < 0) {
            if (errno == EINTR || errno == ENOENT || errno == EAGAIN) continue;
            /* ENODEV: unmounted, or replaced by a newer session's mount. */
            break;
        }
        uint32_t opcode = length >= 8 ? np_file_u32(buffer + 4) : 0;
        struct waiting *waiting = opcode == FUSE_SETLKW_OPCODE ? malloc(sizeof(*waiting) + (size_t)length) : NULL;
        pthread_t thread;
        if (waiting) {
            waiting->service = service; waiting->length = (size_t)length;
            memcpy(waiting->request, buffer, (size_t)length);
            if (pthread_create(&thread, NULL, serve_waiting, waiting) == 0) { pthread_detach(thread); continue; }
            free(waiting);
        }
        if (np_drop_fs_handle(service->fs, buffer, (size_t)length, write_reply, &service->fuse) < 0)
            fprintf(stderr, "[drops] malformed FUSE request\n");
        if (np_drop_fs_destroyed(service->fs)) break;
    }
    free(buffer);
    return NULL;
}

/* One host session at a time; a reconnecting host replaces the previous one. */
static void *accept_host(void *argument) {
    struct np_drop_service *service = argument;
    for (;;) {
        struct pollfd wait[2] = {{service->listener, POLLIN, 0}, {service->wake[0], POLLIN, 0}};
        if (poll(wait, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (wait[1].revents) break;
        int fd = accept4(service->listener, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) continue;
        if (!np_file_peer_is_host(fd)) { close(fd); continue; }
        struct np_file2_session *session = np_file2_session_start(fd, NP_FILE2_ROLE_GUEST, NULL);
        if (!session) { perror("[drops] session"); continue; }
        if (np_file2_notify(session, NP_DROP_MOUNTED, service->path, strlen(service->path)) < 0) {
            np_file2_session_close(session);
            continue;
        }
        struct np_file2_session *previous = service->session;
        np_drop_fs_set_session(service->fs, session);
        service->session = session;
        np_file2_session_close(previous);
    }
    return NULL;
}

struct np_drop_service *np_drop_service_start(void) {
    struct np_drop_service *service = calloc(1, sizeof(*service));
    if (!service) return NULL;
    unsigned started = 0;
    service->fuse = service->listener = service->wake[0] = service->wake[1] = -1;
    service->fuse = request_mount(service->path, sizeof(service->path));
    if (service->fuse < 0) goto fail;
    service->fs = np_drop_fs_create(getuid(), getgid());
    if (!service->fs) goto fail;
    service->listener = np_file_listen_vsock(NP_FILE2_PORT);
    if (service->listener < 0 || pipe2(service->wake, O_CLOEXEC) < 0) goto fail;
    /* Workers live as long as the mount: a read on /dev/fuse cannot be woken,
     * so they end with the mount or the process, never on stop. */
    for (; started < WORKERS; started++) {
        pthread_t worker;
        if (pthread_create(&worker, NULL, serve_fuse, service) != 0) goto fail;
        pthread_detach(worker);
    }
    if (pthread_create(&service->acceptor, NULL, accept_host, service) != 0) goto fail;
    return service;
fail:;
    int error = errno;
    if (service->listener >= 0) close(service->listener);
    if (service->wake[0] >= 0) { close(service->wake[0]); close(service->wake[1]); }
    /* Started workers still reference the service and keep serving the mount.
     * Without any, close it: a mount with no daemon would hang its users. */
    if (!started) {
        if (service->fuse >= 0) close(service->fuse);
        np_drop_fs_destroy(service->fs);
        free(service);
    }
    errno = error;
    return NULL;
}

void np_drop_service_stop(struct np_drop_service *service) {
    if (!service) return;
    if (service->remote) {
        np_drop_fs_set_session(service->fs, NULL);
        np_file2_session_close(service->session);
        service->session = NULL;
        return;
    }
    (void)!write(service->wake[1], "x", 1);
    pthread_join(service->acceptor, NULL);
    np_drop_fs_set_session(service->fs, NULL);
    np_file2_session_close(service->session);
    service->session = NULL;
    close(service->listener); service->listener = -1;
    /* The mount and its workers remain until the process exits. */
}

/* MARK: Remote computers */

/* Without root, an ordinary user mounts FUSE the way libfuse does: the
 * system's setuid fusermount opens /dev/fuse, mounts, and passes the
 * descriptor back over _FUSE_COMMFD. It runs only for the mount. */
static int find_program(const char *name, char out[PATH_MAX]) {
    const char *search = getenv("PATH");
    if (!search || !*search) search = "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";
    while (*search) {
        const char *end = strchr(search, ':');
        size_t length = end ? (size_t)(end - search) : strlen(search);
        if (length && length + 1 + strlen(name) < PATH_MAX) {
            memcpy(out, search, length); out[length] = '/';
            strcpy(out + length + 1, name);
            if (access(out, X_OK) == 0) return 0;
        }
        search += length + (end ? 1 : 0);
    }
    return -1;
}

static int run(const char *tool, char *const argv[], char *const envp[]) {
    pid_t pid;
    if (posix_spawn(&pid, tool, NULL, NULL, argv, envp)) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int fusermount(const char *mountpoint) {
    char tool[PATH_MAX];
    if (find_program("fusermount3", tool) < 0 && find_program("fusermount", tool) < 0) { errno = ENOENT; return -1; }
    /* A previous compositor's mount, alive or aborted, gives way to this one. */
    struct statfs filesystem;
    if (statfs(mountpoint, &filesystem) == 0 && filesystem.f_type == 0x65735546) {
        char *const unmount[] = {tool, "-u", "-z", "--", (char *)mountpoint, NULL};
        (void)run(tool, unmount, environ);
    }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) return -1;
    (void)fcntl(sockets[1], F_SETFD, 0);
    size_t count = 0;
    while (environ[count]) count++;
    char **envp = calloc(count + 2, sizeof(*envp));
    char commfd[32];
    snprintf(commfd, sizeof(commfd), "_FUSE_COMMFD=%d", sockets[1]);
    if (!envp) { close(sockets[0]); close(sockets[1]); errno = ENOMEM; return -1; }
    size_t used = 0;
    for (size_t i = 0; i < count; i++) if (strncmp(environ[i], "_FUSE_COMMFD=", 13)) envp[used++] = environ[i];
    envp[used++] = commfd;
    char options[] = "nosuid,nodev,default_permissions,fsname=" NP_DROP_FS_NAME ",subtype=nativepipe,max_read=131072";
    char *const argv[] = {tool, "-o", options, "--", (char *)mountpoint, NULL};
    pid_t pid;
    int spawned = posix_spawn(&pid, tool, NULL, NULL, argv, envp);
    free(envp);
    close(sockets[1]);
    int fuse = -1;
    if (!spawned) {
        char byte;
        struct iovec vector = {&byte, 1};
        union { struct cmsghdr header; char space[CMSG_SPACE(sizeof(int))]; } control;
        struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1,
                                 .msg_control = control.space, .msg_controllen = sizeof(control.space)};
        ssize_t received;
        do received = recvmsg(sockets[0], &message, MSG_CMSG_CLOEXEC); while (received < 0 && errno == EINTR);
        struct cmsghdr *header = received > 0 ? CMSG_FIRSTHDR(&message) : NULL;
        if (header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS)
            memcpy(&fuse, CMSG_DATA(header), sizeof(int));
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    close(sockets[0]);
    if (fuse < 0) errno = spawned ? spawned : EIO;
    return fuse;
}

struct np_drop_service *np_drop_service_start_remote(int socket) {
    struct np_drop_service *service = calloc(1, sizeof(*service));
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!service || !runtime || runtime[0] != '/' ||
        (size_t)snprintf(service ? service->path : NULL, service ? sizeof(service->path) : 0,
                         "%s/nativepipe-drops", runtime) >= sizeof(service->path)) {
        free(service); close(socket); errno = ENOENT; return NULL;
    }
    service->remote = 1;
    service->listener = service->wake[0] = service->wake[1] = -1;
    if (mkdir(service->path, 0700) < 0 && errno != EEXIST) goto fail;
    service->fuse = fusermount(service->path);
    if (service->fuse < 0) goto fail;
    service->fs = np_drop_fs_create(getuid(), getgid());
    if (!service->fs) goto fail_mount;
    unsigned started = 0;
    for (; started < WORKERS; started++) {
        pthread_t worker;
        if (pthread_create(&worker, NULL, serve_fuse, service) != 0) break;
        pthread_detach(worker);
    }
    if (!started) goto fail_mount;
    service->session = np_file2_session_start(socket, NP_FILE2_ROLE_GUEST, NULL);
    socket = -1;
    if (!service->session || np_file2_notify(service->session, NP_DROP_MOUNTED, service->path, strlen(service->path)) < 0) {
        /* Workers keep the (empty) mount served; without a host it lists nothing. */
        np_file2_session_close(service->session);
        service->session = NULL;
        return service;
    }
    np_drop_fs_set_session(service->fs, service->session);
    return service;
fail_mount:
    close(service->fuse);
fail:;
    int error = errno;
    if (socket >= 0) close(socket);
    np_drop_fs_destroy(service->fs);
    free(service);
    errno = error;
    return NULL;
}
#endif
