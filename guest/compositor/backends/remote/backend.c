#define _GNU_SOURCE
#include "backend.h"
#include "backend_internal.h"
#include "compositor.h"
#include "dmabuf.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int np_backend_run(int argc, char **argv)
{
    bool session = argc == 3 && !strcmp(argv[1], "--stdio") && !strcmp(argv[2], "--session");
    if (!session && (argc < 4 || strcmp(argv[1], "--stdio") || strcmp(argv[2], "--"))) {
        fprintf(stderr, "Usage: nativepipe-wayland --stdio -- application [arguments...]\n");
        return 2;
    }
    /* Keep the SSH streams away from dbus-daemon and activated services.
     * dbus-run-session preserves the extra fds for this executable, while
     * its own standard streams are /dev/null and stderr. */
    const char *streams = getenv("NP_REMOTE_STREAM_FDS");
    int input_fd = -1, output_fd = -1;
    if (!streams) {
        input_fd = fcntl(STDIN_FILENO, F_DUPFD, 3);
        output_fd = fcntl(STDOUT_FILENO, F_DUPFD, 3);
        int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        char fds[64];
        snprintf(fds, sizeof(fds), "%d:%d", input_fd, output_fd);
        if (input_fd < 0 || output_fd < 0 || null_fd < 0 ||
            setenv("NP_REMOTE_STREAM_FDS", fds, 1) < 0 ||
            setenv("NP_PRIVATE_APPLICATION_BUS", "1", 1) < 0 ||
            dup2(null_fd, STDIN_FILENO) < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
            perror("[wayland] private session streams"); return 1;
        }
        close(null_fd);
        /* Never activate a singleton on the SSH user's physical desktop bus. */
        char **command = calloc((size_t)argc + 4, sizeof(char *));
        if (!command) return 1;
        command[0] = "dbus-run-session"; command[1] = "--";
        for (int i = 0; i < argc; ++i) command[i + 2] = argv[i];
        execvp(command[0], command);
        perror("[wayland] dbus-run-session"); free(command); return 1;
    }
    char trailing;
    if (sscanf(streams, "%d:%d%c", &input_fd, &output_fd, &trailing) != 2 ||
        input_fd < 3 || output_fd < 3 || input_fd == output_fd ||
        fcntl(output_fd, F_SETFD, FD_CLOEXEC) < 0 || dup2(input_fd, STDIN_FILENO) < 0) {
        fprintf(stderr, "[wayland] invalid private session streams\n"); return 1;
    }
    close(input_fd);
    unsetenv("NP_REMOTE_STREAM_FDS");
    /* Each invocation owns its display, Xauthority and lifetime. No stale
     * readiness/PID files shared with a second SSH invocation. */
    char runtime[] = "/tmp/nativepipe-XXXXXX";
    if (!mkdtemp(runtime) || setenv("XDG_RUNTIME_DIR", runtime, 1) < 0) return 1;
    /* signalfd handlers must own signals before EGL/encoder worker threads
     * inherit their masks. Application children restore an empty mask. */
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGCHLD); sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGHUP); sigaddset(&signals, SIGINT);
    if (sigprocmask(SIG_BLOCK, &signals, NULL) < 0) return 1;
    struct np_remote_backend backend = { .application_fd = -1 };
    backend.output_fd = output_fd;
    backend.command = session ? NULL : argv + 3;
    /* All library/app logs go to stderr; only the writer owns protocol stdout. */
    int result = np_frontend_run(argc, argv, &backend);
    rmdir(runtime);
    return result ? result : backend.exit_status;
}
bool np_backend_prepare(struct np_server *server)
{
    fprintf(stderr, "[wayland] remote backend: SSH stdio, H.264/alpha resources\n");
    return true;
}
void np_backend_advertise_globals(struct np_server *server)
{
    np_dmabuf_advertise(server->display, -1);
}
void np_backend_finish(struct np_server *server)
{
    struct np_remote_backend *b = np_remote_backend(server);
    fprintf(stderr, "[remote] frames captured=%llu coalesced-before-encode=%llu encoded=%llu scenes=%llu displayed=%llu discarded=%llu\n",
        (unsigned long long)b->captured_frames, (unsigned long long)b->coalesced_frames,
        (unsigned long long)b->encoded_frames, (unsigned long long)b->sent_scenes,
        (unsigned long long)b->displayed_scenes, (unsigned long long)b->discarded_scenes);
    fprintf(stderr, "[remote] transport pending-display=%zu pending-all=%zu flight=%zu budget=%zu\n",
        b->media.display_bytes, b->media.queued_bytes, b->media.flow.bytes, b->media.flow.window);
    free(b->input_record);
}
