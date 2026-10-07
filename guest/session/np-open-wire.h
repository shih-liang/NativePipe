#ifndef NATIVEPIPE_OPEN_WIRE_H
#define NATIVEPIPE_OPEN_WIRE_H

/*
 * Wire format and argument handling of np-open, the guest tool that asks the Mac
 * to open a URL or a file. Everything here is portable C so it can be tested
 * without a guest: np-open.c adds the Unix/vsock connection.
 *
 * Request  (guest to host), little-endian:
 *   "NPOP" | version u8 | kind u8 | reserved u16 = 0 | length u32 | payload
 * Response (host to guest):
 *   "NPOR" | version u8 | status u8 | reserved u16 = 0 | length u32 | message
 * The payload is UTF-8 without NUL; the message is text for the user.
 */

#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NP_OPEN_MAGIC_REQUEST "NPOP"
#define NP_OPEN_MAGIC_RESPONSE "NPOR"
#define NP_OPEN_VERSION 1u
#define NP_OPEN_HEADER_SIZE 12u
#define NP_OPEN_MAX_PAYLOAD 8192u
#define NP_OPEN_MAX_MESSAGE 512u

enum np_open_kind {
	NP_OPEN_KIND_URL = 1,
	NP_OPEN_KIND_FILE = 2,
};

enum np_open_status {
	NP_OPEN_STATUS_OPENED = 0,
	NP_OPEN_STATUS_REFUSED = 1,  /* the host's policy does not allow it */
	NP_OPEN_STATUS_FAILED = 2,   /* allowed, but it could not be opened */
	NP_OPEN_STATUS_DISABLED = 3, /* forwarding is switched off on the Mac */
};

static inline void np_open_put_u32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
	out[2] = (uint8_t)(value >> 16);
	out[3] = (uint8_t)(value >> 24);
}

static inline uint32_t np_open_get_u32(const uint8_t *in)
{
	return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
	       ((uint32_t)in[3] << 24);
}

/* Writes one request frame and returns its length, or 0 when the value is
 * empty, too long, or holds a control character (a NUL or a newline cannot be
 * part of a path or URL a user meant to open). */
static inline size_t np_open_encode_request(uint8_t kind, const char *value,
                                            uint8_t *out, size_t capacity)
{
	if (!value || (kind != NP_OPEN_KIND_URL && kind != NP_OPEN_KIND_FILE)) return 0;
	size_t length = strlen(value);
	if (length == 0 || length > NP_OPEN_MAX_PAYLOAD ||
	    capacity < NP_OPEN_HEADER_SIZE + length)
		return 0;
	for (size_t i = 0; i < length; i++)
		if ((unsigned char)value[i] < 0x20 || value[i] == 0x7f) return 0;
	memcpy(out, NP_OPEN_MAGIC_REQUEST, 4);
	out[4] = (uint8_t)NP_OPEN_VERSION;
	out[5] = kind;
	out[6] = out[7] = 0;
	np_open_put_u32(out + 8, (uint32_t)length);
	memcpy(out + NP_OPEN_HEADER_SIZE, value, length);
	return NP_OPEN_HEADER_SIZE + length;
}

/* Validates a response header. Returns 0 and fills status and message length,
 * or -1 when it is not a response this tool understands. */
static inline int np_open_parse_response_header(const uint8_t *header, uint8_t *status,
                                                uint32_t *length)
{
	if (memcmp(header, NP_OPEN_MAGIC_RESPONSE, 4) != 0 || header[4] != NP_OPEN_VERSION ||
	    header[6] != 0 || header[7] != 0 || header[5] > NP_OPEN_STATUS_DISABLED)
		return -1;
	uint32_t size = np_open_get_u32(header + 8);
	if (size > NP_OPEN_MAX_MESSAGE) return -1;
	*status = header[5];
	*length = size;
	return 0;
}

/* RFC 3986 scheme: a letter, then letters, digits, "+", "-" or "." and a colon.
 * A path such as "a/b:c" has a slash first and so is not a URL. */
static inline int np_open_looks_like_url(const char *argument)
{
	if (!argument || !isalpha((unsigned char)argument[0])) return 0;
	size_t i = 1;
	while (isalnum((unsigned char)argument[i]) || argument[i] == '+' ||
	       argument[i] == '-' || argument[i] == '.')
		i++;
	return argument[i] == ':';
}

static inline int np_open_hex_value(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Converts file:///path and file://localhost/path to a percent-decoded path.
 * Returns 0 on success, -1 for a non-file URL, another host, or a bad escape. */
static inline int np_open_file_url_to_path(const char *url, char *out, size_t capacity)
{
	if (!url || capacity == 0 || strncasecmp(url, "file://", 7) != 0) return -1;
	const char *rest = url + 7;
	if (strncasecmp(rest, "localhost/", 10) == 0) rest += 9;
	else if (rest[0] != '/') return -1;
	size_t length = 0;
	for (const char *p = rest; *p; p++) {
		if (*p == '?' || *p == '#') break; /* a query or fragment is not part of the path */
		char c = *p;
		if (c == '%') {
			int high = p[1] ? np_open_hex_value(p[1]) : -1;
			int low = high >= 0 && p[2] ? np_open_hex_value(p[2]) : -1;
			if (high < 0 || low < 0) return -1;
			c = (char)((high << 4) | low);
			p += 2;
			if (c == '\0') return -1;
		}
		if (length + 1 >= capacity) return -1;
		out[length++] = c;
	}
	out[length] = '\0';
	return length > 0 ? 0 : -1;
}

#endif
