/* Host-runnable checks of np-open's wire format and argument handling. */
#include "np-open-wire.h"

#include <assert.h>
#include <stdio.h>

static void hex(const uint8_t *data, size_t n)
{
	for (size_t i = 0; i < n; i++) printf("%02x", data[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	(void)argv;
	uint8_t frame[NP_OPEN_HEADER_SIZE + NP_OPEN_MAX_PAYLOAD];

	/* Golden frames: the Swift HostOpenWire tests decode exactly these bytes. */
	size_t length = np_open_encode_request(NP_OPEN_KIND_URL, "https://example.com/a?b=1", frame, sizeof(frame));
	assert(length == NP_OPEN_HEADER_SIZE + 25);
	if (argc > 1) { printf("url:"); hex(frame, length); }
	assert(memcmp(frame, "NPOP", 4) == 0 && frame[4] == 1 && frame[5] == NP_OPEN_KIND_URL);
	assert(np_open_get_u32(frame + 8) == 25);
	length = np_open_encode_request(NP_OPEN_KIND_FILE, "/home/dev/\xE6\x96\x87\xE6\xA1\xA3.pdf", frame, sizeof(frame));
	if (argc > 1) { printf("file:"); hex(frame, length); }
	assert(frame[5] == NP_OPEN_KIND_FILE);

	/* Refused before sending: empty, oversize, control characters, bad kind. */
	assert(np_open_encode_request(NP_OPEN_KIND_URL, "", frame, sizeof(frame)) == 0);
	assert(np_open_encode_request(NP_OPEN_KIND_URL, NULL, frame, sizeof(frame)) == 0);
	assert(np_open_encode_request(7, "x", frame, sizeof(frame)) == 0);
	assert(np_open_encode_request(NP_OPEN_KIND_URL, "https://a/\nb", frame, sizeof(frame)) == 0);
	assert(np_open_encode_request(NP_OPEN_KIND_URL, "https://a/\x7f", frame, sizeof(frame)) == 0);
	assert(np_open_encode_request(NP_OPEN_KIND_URL, "https://example.com", frame, 8) == 0);
	static char big[NP_OPEN_MAX_PAYLOAD + 2];
	memset(big, 'a', sizeof(big) - 1);
	assert(np_open_encode_request(NP_OPEN_KIND_FILE, big, frame, sizeof(frame)) == 0);
	big[NP_OPEN_MAX_PAYLOAD] = '\0';
	assert(np_open_encode_request(NP_OPEN_KIND_FILE, big, frame, sizeof(frame)) == NP_OPEN_HEADER_SIZE + NP_OPEN_MAX_PAYLOAD);

	/* Response headers. */
	uint8_t header[NP_OPEN_HEADER_SIZE] = {'N', 'P', 'O', 'R', 1, NP_OPEN_STATUS_REFUSED, 0, 0, 0x1c, 0, 0, 0};
	uint8_t status;
	uint32_t size;
	assert(np_open_parse_response_header(header, &status, &size) == 0);
	assert(status == NP_OPEN_STATUS_REFUSED && size == 28);
	header[0] = 'X';
	assert(np_open_parse_response_header(header, &status, &size) < 0);
	header[0] = 'N'; header[4] = 2;
	assert(np_open_parse_response_header(header, &status, &size) < 0);
	header[4] = 1; header[5] = 9;
	assert(np_open_parse_response_header(header, &status, &size) < 0);
	header[5] = 0; np_open_put_u32(header + 8, NP_OPEN_MAX_MESSAGE + 1);
	assert(np_open_parse_response_header(header, &status, &size) < 0);
	header[6] = 1; np_open_put_u32(header + 8, 0);
	assert(np_open_parse_response_header(header, &status, &size) < 0);

	/* URL or path? */
	assert(np_open_looks_like_url("https://example.com"));
	assert(np_open_looks_like_url("mailto:a@b.c"));
	assert(np_open_looks_like_url("x-scheme+handler.v1:rest"));
	assert(np_open_looks_like_url("a:custom"));
	assert(!np_open_looks_like_url("/usr/share/doc"));
	assert(!np_open_looks_like_url("report.pdf"));
	assert(!np_open_looks_like_url("dir/a:b"));
	assert(np_open_looks_like_url("C:\\x"));
	assert(!np_open_looks_like_url("1http://x"));
	assert(!np_open_looks_like_url(""));
	assert(!np_open_looks_like_url(NULL));

	/* file: URLs become paths; anything else is refused. */
	char path[256];
	assert(np_open_file_url_to_path("file:///home/dev/My%20Doc.txt", path, sizeof(path)) == 0);
	assert(strcmp(path, "/home/dev/My Doc.txt") == 0);
	assert(np_open_file_url_to_path("FILE://localhost/tmp/a", path, sizeof(path)) == 0 && strcmp(path, "/tmp/a") == 0);
	assert(np_open_file_url_to_path("file:///a/b?x=1#frag", path, sizeof(path)) == 0 && strcmp(path, "/a/b") == 0);
	assert(np_open_file_url_to_path("file://otherhost/etc/passwd", path, sizeof(path)) < 0);
	assert(np_open_file_url_to_path("file:relative", path, sizeof(path)) < 0);
	assert(np_open_file_url_to_path("file:///bad%zz", path, sizeof(path)) < 0);
	assert(np_open_file_url_to_path("file:///trunc%4", path, sizeof(path)) < 0);
	assert(np_open_file_url_to_path("file:///nul%00byte", path, sizeof(path)) < 0);
	assert(np_open_file_url_to_path("file:///", path, 2) < 0 || path[0] == '/');
	assert(np_open_file_url_to_path("https://x/y", path, sizeof(path)) < 0);
	assert(np_open_file_url_to_path("file:///long/path", path, 5) < 0);
	assert(np_open_file_url_to_path(NULL, path, sizeof(path)) < 0);

	puts("np-open: ok");
	return 0;
}
