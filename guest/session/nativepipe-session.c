/*
 * nativepipe-session — empty display manager (SDDM-shaped, no UI).
 *
 * systemd starts this as root before a login prompt. There is no greeter
 * surface (NativePipe has no DRM scanout). Once guestd writes the host-chosen
 * user and the compositor binary is on disk, this process:
 *   1. stays root (like a display manager)
 *   2. opens a session as that user (setgid/setuid, no PAM yet)
 *   3. execs the compositor in the child
 *
 * Auto-login is the only mode. Console getty is left alone.
 */
#include "np.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t child_pid = -1;
static pid_t session_services[3] = {-1, -1, -1};

static void die(const char *msg) {
    fprintf(stderr, "[session] %s\n", msg);
    exit(1);
}

static void on_signal(int sig) {
    if (child_pid > 0) {
        /* The child calls setsid() before dbus-run-session.  Signal the whole
         * graphical session, not just that wrapper; otherwise the compositor
         * is reparented to PID 1 and keeps the Wayland lock and vsock port. */
        kill(-child_pid, sig);
        kill(child_pid, sig);
    }
    for (size_t i = 0; i < sizeof(session_services) / sizeof(session_services[0]); i++) {
        if (session_services[i] > 0)
            kill(session_services[i], sig);
    }
    if (sig == SIGTERM || sig == SIGINT)
        _exit(0);
}

static const char *first_executable(const char *const *paths) {
    for (size_t i = 0; paths[i]; i++) {
        if (access(paths[i], X_OK) == 0)
            return paths[i];
    }
    return NULL;
}

static pid_t spawn_service(const char *const *paths) {
    const char *program = first_executable(paths);
    if (!program)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {(char *)program, NULL};
        execv(program, argv);
        _exit(127);
    }
    return pid;
}

static void apply_desktop_preferences(void) {
    FILE *file = fopen(NP_DESKTOP_PREFERENCES_FILE, "r");
    if (!file)
        return;
    char line[64];
    int have_line = fgets(line, sizeof(line), file) != NULL;
    fclose(file);
    int dark = have_line && strcmp(line, "color-scheme=dark\n") == 0;
    int light = have_line && strcmp(line, "color-scheme=light\n") == 0;
    if (!dark && !light)
        return;

    static const char *const paths[] = {"/usr/bin/gsettings", "/bin/gsettings", NULL};
    const char *program = first_executable(paths);
    if (!program)
        return;
    char *scheme[] = {(char *)program, "set", "org.gnome.desktop.interface",
                      "color-scheme", dark ? "prefer-dark" : "prefer-light", NULL};
    char *theme[] = {(char *)program, "set", "org.gnome.desktop.interface",
                     "gtk-theme", dark ? "Adwaita-dark" : "Adwaita", NULL};
    if (np_run(scheme) != 0 || np_run(theme) != 0)
        fprintf(stderr, "[session] could not apply desktop appearance\n");
}

static void start_audio_services(void) {
    /* The installer enables the packaged user services on systemd guests.
     * Leave both startup and lifetime management to the user manager. */
    if (np_path_exists("/run/systemd/system"))
        return;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char pipewire_socket[256];
    pipewire_socket[0] = '\0';
    if (runtime && runtime[0])
        snprintf(pipewire_socket, sizeof(pipewire_socket), "%s/pipewire-0", runtime);

    if (!pipewire_socket[0] || !np_path_exists(pipewire_socket)) {
        static const char *const pipewire[] = {
            "/usr/bin/pipewire", "/bin/pipewire", NULL};
        static const char *const pulse[] = {
            "/usr/bin/pipewire-pulse", "/bin/pipewire-pulse", NULL};
        static const char *const wireplumber[] = {
            "/usr/bin/wireplumber", "/bin/wireplumber", NULL};
        session_services[0] = spawn_service(pipewire);
        if (session_services[0] > 0) {
            session_services[1] = spawn_service(pulse);
            session_services[2] = spawn_service(wireplumber);
        }
    }
}

/* Runs inside dbus-run-session after privileges have already been dropped.
 * On non-systemd guests, own the ordinary audio processes for this session. */
static int user_session_main(void) {
    start_audio_services();
    apply_desktop_preferences();

    child_pid = fork();
    if (child_pid == 0) {
        char *argv[] = {NP_INSTALLED_COMPOSITOR, NULL};
        execv(NP_INSTALLED_COMPOSITOR, argv);
        _exit(127);
    }
    if (child_pid < 0)
        return 1;
    int status = 0;
    while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {}
    child_pid = -1;
    for (size_t i = 0; i < sizeof(session_services) / sizeof(session_services[0]); i++) {
        if (session_services[i] > 0) {
            kill(session_services[i], SIGTERM);
            waitpid(session_services[i], NULL, 0);
            session_services[i] = -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void strip_nl(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static int read_session_user(char *out, size_t cap) {
    FILE *f = fopen(NP_SESSION_USER_FILE, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    strip_nl(out);
    return out[0] ? 0 : -1;
}

static int wait_path(const char *path, int seconds) {
    for (int i = 0; i < seconds; i++) {
        if (np_path_exists(path))
            return 0;
        sleep(1);
    }
    return -1;
}

static int setup_runtime(const struct passwd *pw, char *runtime, size_t cap) {
    snprintf(runtime, cap, "/run/user/%d", (int)pw->pw_uid);

    /* A linger-enabled systemd user manager recreates /run/user/$UID during
     * boot.  Starting the compositor before user-runtime-dir@UID finishes
     * leaves it listening on an unlinked Wayland socket.  A display manager
     * would normally get this ordering from pam_systemd; this deliberately
     * small session launcher waits for the equivalent user-manager marker. */
    char manager[192];
    snprintf(manager, sizeof(manager), "%s/systemd", runtime);
    if (np_path_exists("/run/systemd/system") &&
        access("/usr/bin/loginctl", X_OK) == 0) {
        for (int i = 0; i < 500 && !np_path_exists(manager); i++)
            usleep(20000);
    }

    if (np_mkdir_p(runtime) < 0 && !np_path_exists(runtime))
        return -1;
    if (chown(runtime, pw->pw_uid, pw->pw_gid) < 0)
        return -1;
    if (chmod(runtime, 0700) < 0)
        return -1;
    return 0;
}

static int drop_and_exec(const struct passwd *pw, const char *runtime) {
    if (initgroups(pw->pw_name, pw->pw_gid) < 0)
        return -1;
    if (setgid(pw->pw_gid) < 0)
        return -1;
    if (setuid(pw->pw_uid) < 0)
        return -1;
    if (pw->pw_dir && pw->pw_dir[0])
        chdir(pw->pw_dir);

    char userenv[96], lognameenv[96], homeenv[320], shellenv[192];
    char pathenv[] = "PATH=/usr/local/bin:/usr/bin:/bin";
    char runtimeenv[192], typeenv[] = "XDG_SESSION_TYPE=wayland";
    char classenv[] = "XDG_SESSION_CLASS=user";
    char desktopenv[] = "XDG_SESSION_DESKTOP=nativepipe";
    char currentdesktopenv[] = "XDG_CURRENT_DESKTOP=NativePipe";
    char preloadenv[] = "LD_PRELOAD=" NP_INSTALLED_ALIGN_BLOB;
    char alignmentenv[] = "NATIVEPIPE_BLOB_ALIGNMENT=16384";
    snprintf(userenv, sizeof(userenv), "USER=%s", pw->pw_name);
    snprintf(lognameenv, sizeof(lognameenv), "LOGNAME=%s", pw->pw_name);
    snprintf(homeenv, sizeof(homeenv), "HOME=%s",
             pw->pw_dir && pw->pw_dir[0] ? pw->pw_dir : "/");
    snprintf(shellenv, sizeof(shellenv), "SHELL=%s",
             pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : "/bin/sh");
    snprintf(runtimeenv, sizeof(runtimeenv), "XDG_RUNTIME_DIR=%s", runtime);
    char *envp[19];
    size_t envc = 0;
    long page_size = sysconf(_SC_PAGESIZE);
    int has_venus = np_venus_icd_available();
    envp[envc++] = userenv;
    envp[envc++] = lognameenv;
    envp[envc++] = homeenv;
    envp[envc++] = shellenv;
    envp[envc++] = pathenv;
    envp[envc++] = runtimeenv;
    envp[envc++] = typeenv;
    envp[envc++] = classenv;
    envp[envc++] = desktopenv;
    envp[envc++] = currentdesktopenv;
    if (page_size > 0 && page_size < 16384 &&
        access(NP_INSTALLED_ALIGN_BLOB, R_OK) == 0)
        envp[envc++] = preloadenv;
    if (has_venus && page_size > 0 && page_size < 16384 &&
        access(NP_INSTALLED_VULKAN_LAYER, R_OK) == 0 &&
        access(NP_INSTALLED_VULKAN_LAYER_MANIFEST, R_OK) == 0)
        envp[envc++] = alignmentenv;
    envp[envc] = NULL;
    if (access("/usr/bin/dbus-run-session", X_OK) == 0) {
        char *dbus_argv[] = {
            "/usr/bin/dbus-run-session", "--", NP_INSTALLED_SESSION,
            "--user-session", NULL};
        execve(dbus_argv[0], dbus_argv, envp);
    }
    char *argv[] = {NP_INSTALLED_SESSION, "--user-session", NULL};
    execve(NP_INSTALLED_SESSION, argv, envp);
    return -1;
}

static int start_compositor(const char *user) {
    struct passwd *pw = getpwnam(user);
    if (!pw) {
        fprintf(stderr, "[session] no such user %s\n", user);
        return -1;
    }
    if (pw->pw_uid == 0) {
        fprintf(stderr, "[session] refusing to run compositor as root\n");
        return -1;
    }
    if (!np_path_exists(NP_INSTALLED_COMPOSITOR)) {
        fprintf(stderr, "[session] compositor not installed yet\n");
        return -1;
    }

    char runtime[128];
    if (setup_runtime(pw, runtime, sizeof(runtime)) < 0) {
        fprintf(stderr, "[session] XDG_RUNTIME_DIR %s failed: %s\n", runtime,
                strerror(errno));
        return -1;
    }

    /* This file is a readiness record, not persistent configuration.  Remove
     * the previous compositor generation before starting the next one so
     * guestd cannot launch a desktop client against a stale socket name. */
    char session_env[256];
    snprintf(session_env, sizeof(session_env), "%s/nativepipe-wayland.env", runtime);
    if (unlink(session_env) < 0 && errno != ENOENT) {
        fprintf(stderr, "[session] remove stale environment %s failed: %s\n",
                session_env, strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setsid();
        if (drop_and_exec(pw, runtime) < 0) {
            fprintf(stderr, "[session] exec compositor failed: %s\n", strerror(errno));
            _exit(127);
        }
    }
    child_pid = pid;
    fprintf(stderr, "[session] compositor pid %d user %s runtime %s\n", (int)pid, user,
            runtime);
    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    child_pid = -1;
    fprintf(stderr, "[session] compositor exited status %d\n", st);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--user-session") == 0) {
        if (geteuid() == 0)
            die("refusing root user session");
        signal(SIGTERM, on_signal);
        signal(SIGINT, on_signal);
        signal(SIGCHLD, SIG_DFL);
        return user_session_main();
    }
    if (geteuid() != 0)
        die("must run as root (display manager)");

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGCHLD, SIG_DFL);

    fprintf(stderr, "[session] waiting for autologin user and compositor\n");
    for (;;) {
        if (wait_path(NP_SESSION_USER_FILE, 3600) < 0)
            continue;
        if (wait_path(NP_INSTALLED_COMPOSITOR, 30) < 0) {
            sleep(2);
            continue;
        }
        char user[64];
        if (read_session_user(user, sizeof(user)) < 0) {
            sleep(1);
            continue;
        }
        start_compositor(user);
        sleep(2);
    }
}
