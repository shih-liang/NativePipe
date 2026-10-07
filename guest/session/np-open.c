/*
 * np-open: ask the Mac to open a URL or a file.
 *
 *   np-open https://example.com
 *   np-open ~/Documents/report.pdf
 *
 * The request goes to the Mac over vsock or NativePipe's private Unix socket.
 * The Mac asks the user before it
 * opens or runs anything with the macOS application they chose.
 *
 * Exit status: 0 everything opened, 1 refused or failed, 2 usage, 3 the host
 * could not be reached.
 */
#include "np.h"
#include "np-open-wire.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/vm_sockets.h>
#endif

#define EXIT_REFUSED 1
#define EXIT_USAGE 2
#define EXIT_UNREACHABLE 3

static void usage(FILE *stream)
{
	fputs("usage: np-open <url|file>...\n"
	      "Opens each URL or file with the matching application on the Mac.\n"
	      "The Mac asks for approval before opening links or transferring files.\n"
	      "Files are copied to a stable Mac location, including shared files.\n"
	      "Saving does not run a file; running programs needs separate approval.\n", stream);
}

/* Connect with a deadline, then use a blocking stream while the user decides.
 * The private Unix socket is published by NativePipe's remote compositor;
 * virtual machines instead connect directly to the Mac over vsock. */
static int connect_address(int family, const struct sockaddr *address, socklen_t length)
{
	int fd = socket(family, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
	    fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto fail;
	if (connect(fd, address, length) < 0) {
		if (errno != EINPROGRESS) goto fail;
		struct pollfd waiting = {.fd = fd, .events = POLLOUT};
		int ready;
		do { ready = poll(&waiting, 1, 5000); } while (ready < 0 && errno == EINTR);
		if (ready <= 0) { if (!ready) errno = ETIMEDOUT; goto fail; }
		int error = 0;
		socklen_t size = sizeof(error);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) goto fail;
		if (error) { errno = error; goto fail; }
	}
	if (fcntl(fd, F_SETFL, flags) < 0) goto fail;
	return fd;
fail:
	{
		int error = errno;
		close(fd);
		errno = error;
	}
	return -1;
}

static int connect_host(void)
{
	const char *path = getenv("NATIVEPIPE_OPEN_SOCKET");
	if (path && path[0]) {
		struct sockaddr_un address;
		memset(&address, 0, sizeof(address));
		address.sun_family = AF_UNIX;
		if (strlen(path) >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
		memcpy(address.sun_path, path, strlen(path) + 1);
		return connect_address(AF_UNIX, (struct sockaddr *)&address, sizeof(address));
	}
#if defined(__linux__) && !defined(NP_OPEN_TEST_VSOCK)
	struct sockaddr_vm address;
	memset(&address, 0, sizeof(address));
	address.svm_family = AF_VSOCK;
	address.svm_cid = NP_CID_HOST;
	address.svm_port = NP_PORT_HOST_OPEN;
	return connect_address(AF_VSOCK, (struct sockaddr *)&address, sizeof(address));
#else
	return np_vsock_connect_host(NP_PORT_HOST_OPEN, 1);
#endif
}

/* One request, one connection. Returns an exit status. */
static int send_request(uint8_t kind, const char *value, const char *label)
{
	uint8_t frame[NP_OPEN_HEADER_SIZE + NP_OPEN_MAX_PAYLOAD];
	size_t length = np_open_encode_request(kind, value, frame, sizeof(frame));
	if (!length) {
		fprintf(stderr, "np-open: %s: not a valid %s to open\n", label,
		        kind == NP_OPEN_KIND_URL ? "URL" : "path");
		return EXIT_REFUSED;
	}
	int fd = connect_host();
	if (fd < 0) {
		fprintf(stderr, "np-open: cannot reach the Mac (is NativePipe or LinPortal connected?)\n");
		return EXIT_UNREACHABLE;
	}
	/* Reading waits for the user's approval. The host cancels a pending request
	 * when this process exits, rather than executing it after a client timeout. */
	struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	int status = EXIT_UNREACHABLE;
	uint8_t header[NP_OPEN_HEADER_SIZE];
	uint8_t outcome = 0;
	uint32_t size = 0;
	char message[NP_OPEN_MAX_MESSAGE + 1];
	if (np_write_full(fd, frame, length) < 0 ||
	    np_read_full(fd, header, sizeof(header)) < 0 ||
	    np_open_parse_response_header(header, &outcome, &size) < 0 ||
	    (size && np_read_full(fd, message, size) < 0)) {
		fprintf(stderr, "np-open: the Mac did not answer\n");
		goto done;
	}
	message[size] = '\0';
	if (outcome == NP_OPEN_STATUS_OPENED) {
		if (size) fprintf(stdout, "%s\n", message);
		status = 0;
	} else {
		fprintf(stderr, "np-open: %s: %s\n", label, size ? message : "not opened");
		status = EXIT_REFUSED;
	}
done:
	close(fd);
	return status;
}

static int open_argument(const char *argument)
{
	char path[PATH_MAX];
	if (np_open_looks_like_url(argument)) {
		if (np_open_file_url_to_path(argument, path, sizeof(path)) == 0)
			argument = path; /* a file: URL names a path in this machine */
		else
			return send_request(NP_OPEN_KIND_URL, argument, "link");
	}
	char resolved[PATH_MAX];
	if (!realpath(argument, resolved)) {
		fprintf(stderr, "np-open: %s: %s\n", argument, strerror(errno));
		return EXIT_REFUSED;
	}
	return send_request(NP_OPEN_KIND_FILE, resolved, argument);
}

int main(int argc, char **argv)
{
	/* A disappearing VM host is a reported transport error, not SIGPIPE. */
	signal(SIGPIPE, SIG_IGN);
	if (argc < 2) {
		usage(stderr);
		return EXIT_USAGE;
	}
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage(stdout);
		return 0;
	}
	int result = 0;
	for (int i = 1; i < argc; i++) {
		int status = open_argument(argv[i]);
		if (status == EXIT_UNREACHABLE) return status; /* no point trying the rest */
		if (status > result) result = status;
	}
	return result;
}
