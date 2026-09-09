#define _GNU_SOURCE
#include "applications.h"
#include "backend.h"
#include "backend_internal.h"
#include "compositor_internal.h"
#include "window_events.h"
#include "windowwire.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
extern char **environ;

static int host_readable(int fd, uint32_t mask, void *data)
{
    (void)fd; (void)mask;
    struct np_server *server = data;
    np_host_pump(&np_remote_backend(server)->host, np_applications_handle_command, server);
    np_backend_session_sync(server);
    return 0;
}
static int stop_session(int signal_number, void *data)
{
    (void)signal_number;
    struct np_server *server = data;
    server->terminate = true;
    return 0;
}
static int application_exited(int fd, uint32_t mask, void *data)
{
    (void)fd; (void)mask;
    struct np_server *server = data;
    struct np_remote_backend *b = np_remote_backend(server);
    int status = 0;
    pid_t result;
    do { result = waitpid(b->application_pid, &status, 0); } while (result < 0 && errno == EINTR);
    b->exit_status = result < 0 ? 1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    server->terminate = true;
    return 0;
}
static int output_failed(int fd, uint32_t mask, void *data)
{
    (void)fd; (void)mask;
    struct np_server *server = data;
    server->terminate = true;
    np_remote_backend(server)->exit_status = 1;
    return 0;
}
void np_backend_session_reset_readiness(void) {}
bool np_backend_session_listen(struct np_server *server)
{
    struct np_remote_backend *b = np_remote_backend(server);
    return np_host_listen(&b->host, 0) && np_media_open(&b->media, b->output_fd);
}
bool np_backend_session_set_socket(struct np_server *server, const char *socket)
{
    if (!socket || strlen(socket) >= sizeof(server->session_socket)) return false;
    strcpy(server->session_socket, socket);
    return true;
}
void np_backend_session_attach(struct np_server *server, struct wl_event_loop *loop)
{
    struct np_remote_backend *b = np_remote_backend(server);
    b->host_connection_source = wl_event_loop_add_fd(
        loop, b->host.conn_fd, WL_EVENT_READABLE, host_readable, server);
    wl_event_loop_add_fd(loop, b->media.error_fd, WL_EVENT_READABLE, output_failed, server);
    wl_event_loop_add_signal(loop, SIGTERM, stop_session, server);
    wl_event_loop_add_signal(loop, SIGHUP, stop_session, server);
    wl_event_loop_add_signal(loop, SIGINT, stop_session, server);
    uint32_t ready[] = {(uint32_t)getpid(), NP_WINDOW_PROTOCOL_VERSION};
    server->host_session_ready = true;
    if (!np_window_event_send(server, NP_GUEST_SESSION_STARTED, ready, 2)) {
        server->terminate = true; return;
    }

    if (!b->command) return;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_init(&attr);
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
    int error = posix_spawnp(&b->application_pid, b->command[0], &actions, &attr,
                            b->command, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (error) {
        fprintf(stderr, "[wayland] cannot launch %s: %s\n", b->command[0], strerror(error));
        b->application_pid = 0;
        b->exit_status = error == ENOENT ? 127 : 126;
        server->terminate = true;
    } else {
        /* A process-specific exit fd cannot lose SIGCHLD to Xwayland's
         * independent signalfd or a library worker thread. */
        b->application_fd = syscall(SYS_pidfd_open, b->application_pid, 0);
        if (b->application_fd < 0 || !wl_event_loop_add_fd(loop, b->application_fd,
                WL_EVENT_READABLE, application_exited, server)) {
            fprintf(stderr, "[wayland] cannot watch application exit: %s\n", strerror(errno));
            b->exit_status = 1;
            server->terminate = true;
        }
    }
}
void np_backend_session_sync(struct np_server *server)
{
    if (!np_backend_connected(server)) server->terminate = true;
}
void np_backend_session_finish(struct np_server *server)
{
    struct np_remote_backend *b = np_remote_backend(server);
    if (b->application_pid > 0) {
        kill(-b->application_pid, SIGHUP);
        /* Disconnecting the display below also disconnects Wayland clients. */
        (void)waitpid(b->application_pid, NULL, WNOHANG);
    }
    np_media_finish(&b->media);
    if (b->application_fd >= 0) { close(b->application_fd); b->application_fd = -1; }
    np_host_finish(&b->host);
}
bool np_backend_connected(const struct np_server *server)
{
    struct np_remote_backend *b = np_remote_backend(server);
    return b && np_host_connected(&b->host) && np_media_connected(&b->media);
}
bool np_backend_send_binary(struct np_server *server, const void *payload, size_t length)
{
    return np_media_send_binary(&np_remote_backend(server)->media, payload, length);
}
