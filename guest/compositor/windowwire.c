#include "windowwire.h"

#include <stdlib.h>
#include <string.h>

#define NP_WINDOW_HEADER 8u

static bool reserve(struct np_window_message *message, size_t count) {
	if (!message || !message->ok || count > SIZE_MAX - message->len) return false;
	size_t needed = message->len + count;
	if (needed > NP_WINDOW_MAX_FIELD + 65536u) {
		message->ok = false;
		return false;
	}
	if (needed <= message->cap) return true;
	size_t cap = message->cap ? message->cap : 128u;
	while (cap < needed) {
		if (cap > SIZE_MAX / 2u) {
			message->ok = false;
			return false;
		}
		cap *= 2u;
	}
	unsigned char *grown = realloc(message->data, cap);
	if (!grown) {
		message->ok = false;
		return false;
	}
	message->data = grown;
	message->cap = cap;
	return true;
}

static void append(struct np_window_message *message,
	               const void *bytes, size_t count) {
	if (!reserve(message, count)) return;
	if (count) memcpy(message->data + message->len, bytes, count);
	message->len += count;
}

void np_window_message_init(struct np_window_message *message,
	                         uint8_t direction, uint8_t opcode) {
	if (!message) return;
	memset(message, 0, sizeof(*message));
	message->ok = direction == NP_WINDOW_GUEST_TO_HOST ||
	              direction == NP_WINDOW_HOST_TO_GUEST;
	static const unsigned char magic[4] = {'N', 'P', 'W', '2'};
	append(message, magic, sizeof(magic));
	np_window_put_u8(message, direction);
	np_window_put_u8(message, opcode);
	np_window_put_u16(message, 0);
}

void np_window_message_clear(struct np_window_message *message) {
	if (!message) return;
	free(message->data);
	memset(message, 0, sizeof(*message));
}

void np_window_put_u8(struct np_window_message *message, uint8_t value) {
	append(message, &value, 1);
}

void np_window_put_bool(struct np_window_message *message, bool value) {
	np_window_put_u8(message, value ? 1u : 0u);
}

void np_window_put_u16(struct np_window_message *message, uint16_t value) {
	unsigned char bytes[2] = {
		(unsigned char)value, (unsigned char)(value >> 8),
	};
	append(message, bytes, sizeof(bytes));
}

void np_window_put_u32(struct np_window_message *message, uint32_t value) {
	unsigned char bytes[4] = {
		(unsigned char)value, (unsigned char)(value >> 8),
		(unsigned char)(value >> 16), (unsigned char)(value >> 24),
	};
	append(message, bytes, sizeof(bytes));
}

void np_window_put_i32(struct np_window_message *message, int32_t value) {
	np_window_put_u32(message, (uint32_t)value);
}

void np_window_put_f64(struct np_window_message *message, double value) {
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	np_window_put_u32(message, (uint32_t)bits);
	np_window_put_u32(message, (uint32_t)(bits >> 32));
}

void np_window_put_bytes(struct np_window_message *message,
	                      const unsigned char *bytes, size_t length) {
	if (!message || length > NP_WINDOW_MAX_FIELD || length > UINT32_MAX ||
	    (length && !bytes)) {
		if (message) message->ok = false;
		return;
	}
	np_window_put_u32(message, (uint32_t)length);
	append(message, bytes, length);
}

void np_window_put_optional_bytes(struct np_window_message *message,
	                               const unsigned char *bytes, size_t length,
	                               bool present) {
	if (!present) {
		np_window_put_u32(message, UINT32_MAX);
		return;
	}
	np_window_put_bytes(message, bytes, length);
}

void np_window_put_string(struct np_window_message *message, const char *value) {
	if (!value) value = "";
	np_window_put_bytes(message, (const unsigned char *)value, strlen(value));
}

static void put_rect(struct np_window_message *message,
	                 const struct np_window_rect *rect) {
	np_window_put_i32(message, rect->x);
	np_window_put_i32(message, rect->y);
	np_window_put_i32(message, rect->width);
	np_window_put_i32(message, rect->height);
}

void np_window_put_frame(struct np_window_message *message,
	                      const struct np_window_frame *frame) {
	if (!message || !frame || frame->damage_count > NP_WINDOW_MAX_COLLECTION ||
	    (frame->damage_count && !frame->damage)) {
		if (message) message->ok = false;
		return;
	}
	np_window_put_u32(message, frame->resource_id);
	np_window_put_i32(message, frame->width);
	np_window_put_i32(message, frame->height);
	np_window_put_i32(message, frame->bytes_per_row);
	np_window_put_i32(message, frame->scale);
	np_window_put_u8(message, frame->format);
	np_window_put_u8(message, frame->source);
	np_window_put_u16(message, frame->bitstream_epoch);
	np_window_put_u32(message, frame->presentation_id);
	uint32_t flags = 0;
	if (frame->has_viewport_source) flags |= 1u << 0;
	if (frame->has_viewport_destination) flags |= 1u << 1;
	if (frame->has_window_geometry) flags |= 1u << 2;
	if (frame->codec) flags |= 1u << 3;
	if (frame->has_gpu_source) flags |= 1u << 4;
	np_window_put_u32(message, flags);
	np_window_put_u32(message, frame->damage_count);
	if (frame->has_viewport_source) {
		np_window_put_f64(message, frame->viewport_source.x);
		np_window_put_f64(message, frame->viewport_source.y);
		np_window_put_f64(message, frame->viewport_source.width);
		np_window_put_f64(message, frame->viewport_source.height);
	}
	if (frame->has_viewport_destination) {
		np_window_put_i32(message, frame->viewport_width);
		np_window_put_i32(message, frame->viewport_height);
	}
	if (frame->has_window_geometry) put_rect(message, &frame->window_geometry);
	if (frame->codec) np_window_put_string(message, frame->codec);
	if (frame->has_gpu_source) np_window_put_u32(message, frame->gpu_source_id);
	for (uint32_t i = 0; i < frame->damage_count; i++) {
		put_rect(message, &frame->damage[i]);
	}
}

static const unsigned char *take(struct np_window_reader *reader, size_t count) {
	if (!reader || !reader->ok || reader->offset > reader->len ||
	    count > reader->len - reader->offset) {
		if (reader) reader->ok = false;
		return NULL;
	}
	const unsigned char *result = reader->data + reader->offset;
	reader->offset += count;
	return result;
}

bool np_window_reader_init(struct np_window_reader *reader,
	                       const unsigned char *data, size_t length,
	                       uint8_t expected_direction) {
	if (!reader) return false;
	memset(reader, 0, sizeof(*reader));
	reader->data = data;
	reader->len = length;
	reader->ok = data && length >= NP_WINDOW_HEADER &&
	             memcmp(data, "NPW2", 4) == 0 &&
	             data[4] == expected_direction &&
	             data[6] == 0 && data[7] == 0;
	if (!reader->ok) return false;
	reader->opcode = data[5];
	reader->offset = NP_WINDOW_HEADER;
	return true;
}

uint8_t np_window_read_u8(struct np_window_reader *reader) {
	const unsigned char *bytes = take(reader, 1);
	return bytes ? bytes[0] : 0;
}

bool np_window_read_bool(struct np_window_reader *reader) {
	uint8_t value = np_window_read_u8(reader);
	if (value > 1 && reader) reader->ok = false;
	return value == 1;
}

uint16_t np_window_read_u16(struct np_window_reader *reader) {
	const unsigned char *bytes = take(reader, 2);
	return bytes ? (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8) : 0;
}

uint32_t np_window_read_u32(struct np_window_reader *reader) {
	const unsigned char *bytes = take(reader, 4);
	return bytes ? (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	               ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24) : 0;
}

int32_t np_window_read_i32(struct np_window_reader *reader) {
	return (int32_t)np_window_read_u32(reader);
}

double np_window_read_fixed(struct np_window_reader *reader) {
	return (double)np_window_read_i32(reader) / 256.0;
}

double np_window_read_f64(struct np_window_reader *reader) {
	uint64_t bits = np_window_read_u32(reader);
	bits |= (uint64_t)np_window_read_u32(reader) << 32;
	double value = 0;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

bool np_window_read_bytes(struct np_window_reader *reader,
	                      const unsigned char **bytes, size_t *length,
	                      bool optional, bool *present) {
	if (bytes) *bytes = NULL;
	if (length) *length = 0;
	if (present) *present = false;
	uint32_t count = np_window_read_u32(reader);
	if (!reader || !reader->ok) return false;
	if (optional && count == UINT32_MAX) return true;
	if (count == UINT32_MAX || count > NP_WINDOW_MAX_FIELD) {
		reader->ok = false;
		return false;
	}
	const unsigned char *value = take(reader, count);
	if (!reader->ok) return false;
	if (bytes) *bytes = value;
	if (length) *length = count;
	if (present) *present = true;
	return true;
}

char *np_window_read_string(struct np_window_reader *reader) {
	const unsigned char *bytes = NULL;
	size_t length = 0;
	bool present = false;
	if (!np_window_read_bytes(reader, &bytes, &length, false, &present) ||
	    !present || length == SIZE_MAX) return NULL;
	char *string = malloc(length + 1u);
	if (!string) {
		reader->ok = false;
		return NULL;
	}
	if (length) memcpy(string, bytes, length);
	string[length] = '\0';
	if (memchr(string, '\0', length)) {
		free(string);
		reader->ok = false;
		return NULL;
	}
	return string;
}

bool np_window_reader_finished(const struct np_window_reader *reader) {
	return reader && reader->ok && reader->offset == reader->len;
}
