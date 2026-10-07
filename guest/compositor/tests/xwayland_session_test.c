/* Exercise the real per-session files without starting an X server. */
#undef NDEBUG
#include "../xwayland.c"
#include <assert.h>

static void prepare(struct np_server *server, int display)
{
	struct np_xwayland *xw = calloc(1, sizeof(*xw));
	assert(xw);
	xw->server = server;
	xw->display = display;
	xw->listen_fd[0] = xw->listen_fd[1] = -1;
	xw->pid = -1;
	server->xwayland = xw;
	assert(create_auth_file(xw));
	snprintf(server->xwayland_auth, sizeof(server->xwayland_auth), "%s", xw->auth_path);
	assert(install_app_defaults(xw));
}

int main(void)
{
	char runtime[] = "/tmp/nativepipe-xwayland-session-XXXXXX";
	assert(mkdtemp(runtime));
	assert(setenv("XDG_RUNTIME_DIR", runtime, 1) == 0);
	struct np_server first = {0}, second = {0};
	prepare(&first, 100);
	prepare(&second, 101);
	assert(strcmp(first.xwayland_auth, second.xwayland_auth));
	assert(strcmp(first.xwayland_appdefaults, second.xwayland_appdefaults));
	struct stat info;
	assert(stat(second.xwayland_auth, &info) == 0 && (info.st_mode & 0777) == 0600);
	assert(stat(second.xwayland_appdefaults, &info) == 0 && (info.st_mode & 0777) == 0700);
	char defaults[300];
	assert(snprintf(defaults, sizeof(defaults), "%s/XTerm", second.xwayland_appdefaults) < (int)sizeof(defaults));
	unsigned char cookie[128];
	int fd = open(second.xwayland_auth, O_RDONLY);
	assert(fd >= 0);
	ssize_t length = read(fd, cookie, sizeof(cookie));
	assert(length > 0 && close(fd) == 0);
	np_xwayland_finish(&first);
	assert(stat(defaults, &info) == 0);
	fd = open(second.xwayland_auth, O_RDONLY);
	assert(fd >= 0);
	unsigned char after[128];
	assert(read(fd, after, sizeof(after)) == length && !memcmp(cookie, after, (size_t)length));
	assert(close(fd) == 0);
	np_xwayland_finish(&second);
	assert(rmdir(runtime) == 0);
	puts("Xwayland concurrent session cookies, defaults and cleanup PASS");
	return 0;
}
