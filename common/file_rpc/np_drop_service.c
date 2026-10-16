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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WORKERS 4u
/* Requests carry at most max_write (4096) bytes; the kernel requires 8 KiB. */
#define REQUEST_BUFFER (64u * 1024u)

struct np_drop_service {
    struct np_drop_fs *fs;
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
    (void)!write(service->wake[1], "x", 1);
    pthread_join(service->acceptor, NULL);
    np_drop_fs_set_session(service->fs, NULL);
    np_file2_session_close(service->session);
    service->session = NULL;
    close(service->listener); service->listener = -1;
    /* The mount and its workers remain until the process exits. */
}
#endif
