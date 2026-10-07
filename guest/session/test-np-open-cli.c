/* Exercise the real CLI with a local socket fixture, never a guest or host app. */
#define NP_OPEN_TEST_VSOCK
#define np_vsock_connect_host test_connect
#define np_write_full test_write_full
#define np_read_full test_read_full
#define main open_tool_main
#include "np-open.c"
#undef main

#include <assert.h>
#include <fcntl.h>
#include <sys/wait.h>

enum outcome { OPENED, REFUSED, CLOSED_BEFORE_WRITE, UNREACHABLE };
static enum outcome outcome;
static unsigned calls;
static uint8_t kind;
static char value[NP_OPEN_MAX_PAYLOAD + 1];
static int unix_transport;

int test_connect(uint32_t port, int retries)
{
	assert(port == NP_PORT_HOST_OPEN && retries == 1);
	if (outcome == UNREACHABLE) return -1;
	int pair[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	if (outcome != CLOSED_BEFORE_WRITE) {
		uint8_t reply[NP_OPEN_HEADER_SIZE] = {'N', 'P', 'O', 'R', 1,
		    outcome == OPENED ? NP_OPEN_STATUS_OPENED : NP_OPEN_STATUS_REFUSED, 0, 0, 0, 0, 0, 0};
		assert(write(pair[1], reply, sizeof(reply)) == (ssize_t)sizeof(reply));
	}
	close(pair[1]);
	return pair[0];
}

int test_write_full(int fd, const void *bytes, size_t size)
{
	const uint8_t *frame = bytes;
	assert(size >= NP_OPEN_HEADER_SIZE && memcmp(frame, "NPOP", 4) == 0);
	size_t length = np_open_get_u32(frame + 8);
	assert(length <= NP_OPEN_MAX_PAYLOAD && size == NP_OPEN_HEADER_SIZE + length);
	kind = frame[5];
	memcpy(value, frame + NP_OPEN_HEADER_SIZE, length);
	value[length] = '\0';
	calls++;
	/* This is a real write to a socket whose peer has gone. With the old CLI
	 * the test process dies with SIGPIPE before it can report exit status 3. */
	if (outcome == CLOSED_BEFORE_WRITE || unix_transport)
		return write(fd, bytes, size) == (ssize_t)size ? 0 : -1;
	return 0;
}

int test_read_full(int fd, void *bytes, size_t size)
{
	size_t offset = 0;
	while (offset < size) {
		ssize_t count = read(fd, (char *)bytes + offset, size - offset);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return -1;
		offset += (size_t)count;
	}
	return 0;
}

static int run(const char *argument)
{
	char *args[] = {"np-open", (char *)argument, NULL};
	return open_tool_main(2, args);
}

static void test_unix_transport(const char *directory)
{
	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	assert(snprintf(address.sun_path, sizeof(address.sun_path), "%s/open.sock", directory) < (int)sizeof(address.sun_path));
	int listener = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(listener >= 0 && bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
	assert(listen(listener, 1) == 0);
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		int client = accept(listener, NULL, NULL);
		assert(client >= 0);
		uint8_t header[NP_OPEN_HEADER_SIZE];
		assert(test_read_full(client, header, sizeof(header)) == 0);
		assert(memcmp(header, "NPOP", 4) == 0 && header[5] == NP_OPEN_KIND_URL);
		uint32_t size = np_open_get_u32(header + 8);
		char payload[NP_OPEN_MAX_PAYLOAD + 1];
		assert(size <= NP_OPEN_MAX_PAYLOAD && test_read_full(client, payload, size) == 0);
		payload[size] = '\0';
		assert(!strcmp(payload, "custom:remote-request"));
		uint8_t reply[NP_OPEN_HEADER_SIZE] = {'N', 'P', 'O', 'R', 1, NP_OPEN_STATUS_OPENED, 0, 0, 0, 0, 0, 0};
		assert(write(client, reply, sizeof(reply)) == (ssize_t)sizeof(reply));
		close(client);
		close(listener);
		_exit(0);
	}
	assert(setenv("NATIVEPIPE_OPEN_SOCKET", address.sun_path, 1) == 0);
	unix_transport = 1;
	assert(run("custom:remote-request") == 0);
	unix_transport = 0;
	assert(unsetenv("NATIVEPIPE_OPEN_SOCKET") == 0);
	close(listener);
	int status = 0;
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(unlink(address.sun_path) == 0);
}

int main(void)
{
	assert(unsetenv("NATIVEPIPE_OPEN_SOCKET") == 0);
	outcome = OPENED;
	assert(run("ssh://user:password@example.test") == 0);
	assert(kind == NP_OPEN_KIND_URL && !strcmp(value, "ssh://user:password@example.test"));
	assert(run("mailto:a@example.test?attach=/tmp/report.pdf") == 0);
	assert(kind == NP_OPEN_KIND_URL);
	assert(run("file://otherhost/shared/report.pdf") == 0 && kind == NP_OPEN_KIND_URL);
	assert(run("a:custom") == 0 && kind == NP_OPEN_KIND_URL && !strcmp(value, "a:custom"));

	char directory[] = "/tmp/nativepipe-open-cli.XXXXXX";
	assert(mkdtemp(directory));
	char previous[PATH_MAX];
	assert(getcwd(previous, sizeof(previous)) && chdir(directory) == 0);
	int file = open("script.sh", O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(file >= 0 && close(file) == 0);
	char expected[PATH_MAX];
	assert(realpath("script.sh", expected));
	assert(run("script.sh") == 0);
	assert(kind == NP_OPEN_KIND_FILE && !strcmp(value, expected));
	test_unix_transport(directory);
	assert(unlink("script.sh") == 0 && chdir(previous) == 0 && rmdir(directory) == 0);

	outcome = REFUSED;
	assert(run("https://example.test") == EXIT_REFUSED);
	outcome = UNREACHABLE;
	assert(run("https://example.test") == EXIT_UNREACHABLE);
	outcome = CLOSED_BEFORE_WRITE;
	signal(SIGPIPE, SIG_DFL);
	assert(run("https://example.test") == EXIT_UNREACHABLE);
	assert(calls == 8);
	puts("np-open CLI: URLs, relative scripts, Unix/vsock selection and disconnected host PASS");
	return 0;
}
