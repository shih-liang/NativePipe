// Lazy X socket activation for the distribution-provided xwayland-satellite.

#define _GNU_SOURCE

#include "xwayland.h"

#include "compositor_internal.h"
#include "user_text.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

#define XWAYLAND_SATELLITE_NAME "xwayland-satellite"

struct np_xwayland {
	struct np_server *server;
	int display;
	int listen_fd[2];
	char socket_path[108];
	char auth_path[256];
	struct wl_event_source *listen_source[2];
	struct wl_event_source *sigchld_source;
	pid_t pid;
};

static bool executable_in_path(const char *name)
{
	const char *path = getenv("PATH");
	if (!name || !name[0]) return false;
	if (!path || !path[0]) path = "/usr/local/bin:/usr/bin:/bin";
	char *copy = strdup(path);
	if (!copy) return false;
	bool found = false;
	char *save = NULL;
	for (char *directory = strtok_r(copy, ":", &save); directory;
	     directory = strtok_r(NULL, ":", &save)) {
		char candidate[4096];
		int length = snprintf(candidate, sizeof(candidate), "%s/%s",
		                      directory, name);
		if (length > 0 && length < (int)sizeof(candidate) &&
		    access(candidate, X_OK) == 0) {
			found = true;
			break;
		}
	}
	free(copy);
	return found;
}

static bool write_all(int fd, const void *bytes, size_t length)
{
	const unsigned char *cursor = bytes;
	while (length) {
		ssize_t written = write(fd, cursor, length);
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0) return false;
		cursor += written;
		length -= (size_t)written;
	}
	return true;
}

static bool write_be16(int fd, uint16_t value)
{
	unsigned char bytes[] = {(unsigned char)(value >> 8), (unsigned char)value};
	return write_all(fd, bytes, sizeof(bytes));
}

static bool random_bytes(void *bytes, size_t length)
{
	unsigned char *cursor = bytes;
	while (length) {
		ssize_t count = getrandom(cursor, length, 0);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return false;
		cursor += count;
		length -= (size_t)count;
	}
	return true;
}

/* Xauthority is big-endian counted data. FamilyWild authorizes both Unix
 * listeners without publishing the guest hostname; the display still has to
 * match the reserved socket number. */
static bool create_auth_file(struct np_xwayland *xw)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	static const char name[] = "MIT-MAGIC-COOKIE-1";
	unsigned char cookie[16];
	char display[16];
	char temporary[256];
	if (!runtime || runtime[0] != '/' ||
	    snprintf(display, sizeof(display), "%d", xw->display) >=
	        (int)sizeof(display) ||
	    snprintf(temporary, sizeof(temporary),
	             "%s/.nativepipe-Xauthority-XXXXXX", runtime) >=
	        (int)sizeof(temporary) ||
	    !random_bytes(cookie, sizeof(cookie)))
		return false;

	int fd = mkstemp(temporary);
	if (fd < 0) return false;
	bool ok = fchmod(fd, 0600) == 0 &&
	          write_be16(fd, UINT16_MAX) && /* FamilyWild */
	          write_be16(fd, 0) &&          /* address */
	          write_be16(fd, (uint16_t)strlen(display)) &&
	          write_all(fd, display, strlen(display)) &&
	          write_be16(fd, (uint16_t)(sizeof(name) - 1)) &&
	          write_all(fd, name, sizeof(name) - 1) &&
	          write_be16(fd, sizeof(cookie)) &&
	          write_all(fd, cookie, sizeof(cookie)) && fsync(fd) == 0;
	if (close(fd) < 0) ok = false;
	/* Each compositor owns its cookie file. A second SSH session must not
	 * replace this one, and closing either session must not delete the other. */
	if (ok) snprintf(xw->auth_path, sizeof(xw->auth_path), "%s", temporary);
	if (!ok) {
		unlink(temporary);
		xw->auth_path[0] = '\0';
	}
	return ok;
}

/* X11 pixels are physical pixels on a scaled display. xterm's default bitmap
 * font ignores Xft.dpi, so it stays tiny while GTK and Qt applications scale.
 * Xft fonts honour the Xft.dpi that xwayland-satellite sets from the display
 * scale, so make them xterm's default. XAPPLRESDIR holds per-user application
 * defaults: they sit below the user's own resources and need no change to
 * distribution packages, and they apply to machines installed earlier too. */
static const char xterm_defaults[] =
	"! Written by NativePipe; replaced at every session start.\n"
	"XTerm*renderFont: true\n"
	"XTerm*faceName: Monospace\n"
	"XTerm*faceSize: 11\n";

#define XWAYLAND_APPDEFAULTS_DIRECTORY "nativepipe-app-defaults-XXXXXX"
#define XWAYLAND_APPDEFAULTS_FILE "XTerm"

static bool install_app_defaults(struct np_xwayland *xw)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char directory[256], temporary[300], path[300];
	if (!runtime || runtime[0] != '/' ||
	    snprintf(directory, sizeof(directory), "%s/%s", runtime,
	             XWAYLAND_APPDEFAULTS_DIRECTORY) >= (int)sizeof(directory) ||
	    snprintf(temporary, sizeof(temporary), "%s/.%s-XXXXXX", directory,
	             XWAYLAND_APPDEFAULTS_FILE) >= (int)sizeof(temporary) ||
	    snprintf(path, sizeof(path), "%s/%s", directory,
	             XWAYLAND_APPDEFAULTS_FILE) >= (int)sizeof(path))
		return false;
	if (!mkdtemp(directory)) return false;
	/* mkdtemp changes the suffix; derive names from the selected directory. */
	snprintf(temporary, sizeof(temporary), "%s/.%s-XXXXXX", directory,
	         XWAYLAND_APPDEFAULTS_FILE);
	snprintf(path, sizeof(path), "%s/%s", directory, XWAYLAND_APPDEFAULTS_FILE);

	int fd = mkstemp(temporary);
	if (fd < 0) { rmdir(directory); return false; }
	bool ok = fchmod(fd, 0644) == 0 &&
	          write_all(fd, xterm_defaults, sizeof(xterm_defaults) - 1) &&
	          fsync(fd) == 0;
	if (close(fd) < 0) ok = false;
	if (ok) ok = rename(temporary, path) == 0;
	if (!ok) {
		unlink(temporary);
		rmdir(directory);
		return false;
	}
	snprintf(xw->server->xwayland_appdefaults,
	         sizeof(xw->server->xwayland_appdefaults), "%s", directory);
	return true;
}

static int make_unix_listener(const char *path, bool abstract)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;

	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	socklen_t length;
	if (abstract) {
		size_t size = strlen(path);
		if (size + 1 >= sizeof(address.sun_path)) {
			close(fd);
			return -1;
		}
		memcpy(address.sun_path + 1, path, size);
		length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + size);
	} else {
		if (strlen(path) >= sizeof(address.sun_path)) {
			close(fd);
			return -1;
		}
		strcpy(address.sun_path, path);
		length = sizeof(address);
	}

	if (bind(fd, (struct sockaddr *)&address, length) < 0 ||
	    listen(fd, 32) < 0 || (!abstract && chmod(path, 0777) < 0)) {
		close(fd);
		if (!abstract) unlink(path);
		return -1;
	}
	return fd;
}

static bool reserve_display(struct np_xwayland *xw)
{
	if (mkdir("/tmp/.X11-unix", 01777) < 0 && errno != EEXIST) return false;
	struct stat directory;
	/* chmod can fail when the conventional directory belongs to root. Its mode
	 * is validated below, so that is harmless for an unprivileged session. */
	(void)chmod("/tmp/.X11-unix", 01777);
	if (stat("/tmp/.X11-unix", &directory) < 0 ||
	    !S_ISDIR(directory.st_mode) ||
	    (directory.st_mode & 01002) != 01002)
		return false;

	for (int display = 0; display < 64; display++) {
		char path[108];
		snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", display);
		struct stat socket_stat;
		if (lstat(path, &socket_stat) == 0) {
			bool stale = S_ISSOCK(socket_stat.st_mode) &&
			             socket_stat.st_uid == geteuid();
			int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
			if (probe >= 0) {
				struct sockaddr_un address;
				memset(&address, 0, sizeof(address));
				address.sun_family = AF_UNIX;
				if (strlen(path) < sizeof(address.sun_path))
					strcpy(address.sun_path, path);
				if (connect(probe, (struct sockaddr *)&address, sizeof(address)) == 0)
					stale = false;
				else if (errno != ECONNREFUSED && errno != ENOENT)
					stale = false;
				close(probe);
			} else stale = false;
			if (!stale || unlink(path) < 0) continue;
		} else if (errno != ENOENT) {
			continue;
		}
		int filesystem = make_unix_listener(path, false);
		if (filesystem < 0) continue;
		int abstract = make_unix_listener(path, true);
		if (abstract < 0) {
			close(filesystem);
			unlink(path);
			continue;
		}

		xw->display = display;
		xw->listen_fd[0] = filesystem;
		xw->listen_fd[1] = abstract;
		snprintf(xw->socket_path, sizeof(xw->socket_path), "%s", path);
		snprintf(xw->server->xwayland_display,
		         sizeof(xw->server->xwayland_display), ":%d", display);
		return true;
	}
	return false;
}

static void remove_listen_sources(struct np_xwayland *xw)
{
	for (size_t i = 0; i < 2; i++) {
		if (xw->listen_source[i]) wl_event_source_remove(xw->listen_source[i]);
		xw->listen_source[i] = NULL;
	}
}

static void add_listen_sources(struct np_xwayland *xw);

static int start_satellite(int fd, uint32_t mask, void *data)
{
	(void)fd;
	(void)mask;
	struct np_xwayland *xw = data;
	if (xw->pid > 0) return 0;
	if (!xw->server->session_socket[0]) {
		np_user_text("X11_FAILED", "X11 applications won’t open: xwayland-satellite couldn’t start (%s).", "the Wayland socket is not ready", NULL);
		return 0;
	}
	remove_listen_sources(xw);

	pid_t pid = fork();
	if (pid == 0) {
		sigset_t signals;
		sigemptyset(&signals);
		sigprocmask(SIG_SETMASK, &signals, NULL);
		for (size_t i = 0; i < 2; i++)
			if (fcntl(xw->listen_fd[i], F_SETFD, 0) < 0) _exit(126);

		char listen0[24], listen1[24];
		snprintf(listen0, sizeof(listen0), "%d", xw->listen_fd[0]);
		snprintf(listen1, sizeof(listen1), "%d", xw->listen_fd[1]);
		setenv("WAYLAND_DISPLAY", xw->server->session_socket, 1);
		unsetenv("WAYLAND_SOCKET");
		execlp(XWAYLAND_SATELLITE_NAME, XWAYLAND_SATELLITE_NAME,
		      xw->server->xwayland_display,
		      "-auth", xw->auth_path,
		      "-nolisten", "tcp",
		      "-glamor", "gl",
		      "-listenfd", listen0,
		      "-listenfd", listen1,
		      (char *)NULL);
		_exit(errno == ENOENT ? 127 : 126);
	}
	if (pid < 0) {
		np_user_text("X11_FAILED", "X11 applications won’t open: xwayland-satellite couldn’t start (%s).", strerror(errno), NULL);
		add_listen_sources(xw);
		return 0;
	}
	xw->pid = pid;
	np_debug_log("[xwayland] satellite pid=%ld starting on %s\n",
	        (long)pid, xw->server->xwayland_display);
	return 0;
}

static void add_listen_sources(struct np_xwayland *xw)
{
	struct wl_event_loop *loop = wl_display_get_event_loop(xw->server->display);
	for (size_t i = 0; i < 2; i++) {
		if (xw->listen_fd[i] < 0 || xw->listen_source[i]) continue;
		xw->listen_source[i] = wl_event_loop_add_fd(
			loop, xw->listen_fd[i], WL_EVENT_READABLE,
			start_satellite, xw);
		if (!xw->listen_source[i]) {
			np_user_text("X11_FAILED", "X11 applications won’t open: xwayland-satellite couldn’t start (%s).", "cannot watch the display socket", NULL);
			remove_listen_sources(xw);
			return;
		}
	}
}

static int sigchld_received(int signal_number, void *data)
{
	(void)signal_number;
	struct np_xwayland *xw = data;
	if (xw->pid <= 0) return 0;
	int status = 0;
	pid_t result = waitpid(xw->pid, &status, WNOHANG);
	if (result != xw->pid) return 0;

	np_debug_log("[xwayland] satellite exited status=%d\n", status);
	xw->pid = -1;
	add_listen_sources(xw);
	return 0;
}

bool np_xwayland_init(struct np_server *server)
{
	if (!server) return false;
	if (!executable_in_path(XWAYLAND_SATELLITE_NAME)) {
		/* Without it every X11 application fails with an opaque "cannot open
		 * display", far from here. Say what is missing while it can be fixed. */
		np_user_text("X11_NOT_INSTALLED", "X11 applications won’t open because xwayland-satellite isn’t installed. Install xwayland-satellite and Xwayland on the Linux computer.", NULL, NULL);
		return false;
	}
	struct np_xwayland *xw = calloc(1, sizeof(*xw));
	if (!xw) return false;
	xw->server = server;
	xw->listen_fd[0] = xw->listen_fd[1] = -1;
	xw->pid = -1;
	server->xwayland = xw;

	if (!reserve_display(xw) || !create_auth_file(xw)) {
		np_user_text("X11_FAILED", "X11 applications won’t open: xwayland-satellite couldn’t start (%s).", "cannot reserve an X11 display", NULL);
		np_xwayland_finish(server);
		return false;
	}
	snprintf(server->xwayland_auth, sizeof(server->xwayland_auth), "%s",
	         xw->auth_path);
	/* Cosmetic: X11 applications still work without these defaults. */
	if (!install_app_defaults(xw))
		np_debug_log("[xwayland] could not install application defaults\n");

	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	xw->sigchld_source = wl_event_loop_add_signal(
		loop, SIGCHLD, sigchld_received, xw);
	if (!xw->sigchld_source) {
		np_user_text("X11_FAILED", "X11 applications won’t open: xwayland-satellite couldn’t start (%s).", "cannot watch the satellite process", NULL);
		np_xwayland_finish(server);
		return false;
	}
	add_listen_sources(xw);
	if (!xw->listen_source[0] || !xw->listen_source[1]) {
		np_xwayland_finish(server);
		return false;
	}

	np_debug_log("[xwayland] satellite display reserved at %s\n",
	        server->xwayland_display);
	return true;
}

static void stop_satellite(struct np_xwayland *xw)
{
	if (xw->pid <= 0) return;
	pid_t pid = xw->pid;
	(void)kill(pid, SIGTERM);
	for (int attempt = 0; attempt < 50; attempt++) {
		pid_t result = waitpid(pid, NULL, WNOHANG);
		if (result == pid || (result < 0 && errno == ECHILD)) {
			xw->pid = -1;
			return;
		}
		usleep(20000);
	}
	(void)kill(pid, SIGKILL);
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
	xw->pid = -1;
}

void np_xwayland_finish(struct np_server *server)
{
	struct np_xwayland *xw = server ? server->xwayland : NULL;
	if (!xw) return;
	remove_listen_sources(xw);
	if (xw->sigchld_source) wl_event_source_remove(xw->sigchld_source);
	xw->sigchld_source = NULL;
	stop_satellite(xw);
	for (size_t i = 0; i < 2; i++) {
		if (xw->listen_fd[i] >= 0) close(xw->listen_fd[i]);
		xw->listen_fd[i] = -1;
	}
	if (xw->socket_path[0]) unlink(xw->socket_path);
	if (xw->auth_path[0]) unlink(xw->auth_path);
	if (server->xwayland_appdefaults[0]) {
		char path[300];
		if (snprintf(path, sizeof(path), "%s/%s", server->xwayland_appdefaults,
		             XWAYLAND_APPDEFAULTS_FILE) < (int)sizeof(path))
			unlink(path);
		rmdir(server->xwayland_appdefaults);
	}
	server->xwayland = NULL;
	server->xwayland_display[0] = '\0';
	server->xwayland_auth[0] = '\0';
	server->xwayland_appdefaults[0] = '\0';
	free(xw);
}

bool np_xwayland_owns_client(struct np_server *server, struct wl_client *client)
{
	struct np_xwayland *xw = server ? server->xwayland : NULL;
	if (!xw || xw->pid <= 0 || !client) return false;
	pid_t pid = -1;
	uid_t uid = 0;
	gid_t gid = 0;
	wl_client_get_credentials(client, &pid, &uid, &gid);
	return pid == xw->pid;
}
