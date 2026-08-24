#include "window_events.h"

#include "compositor_internal.h"
#include "hostlink.h"
#include "windowwire.h"

bool np_window_event_send(struct np_server *server, uint8_t opcode,
                          const uint32_t *values, size_t count) {
	if (!server || count > 16) return false;
	unsigned char payload[8 + 16 * 4] = {
		'N', 'P', 'W', '2', NP_WINDOW_GUEST_TO_HOST, opcode, 0, 0,
	};
	for (size_t i = 0; i < count; i++) {
		uint32_t value = values[i];
		payload[8 + i * 4 + 0] = (unsigned char)value;
		payload[8 + i * 4 + 1] = (unsigned char)(value >> 8);
		payload[8 + i * 4 + 2] = (unsigned char)(value >> 16);
		payload[8 + i * 4 + 3] = (unsigned char)(value >> 24);
	}
	return np_host_send_binary(&server->host, payload, 8 + count * 4);
}

bool np_window_event_send_popup_placement(
    struct np_server *server, uint32_t window, uint32_t parent,
    int32_t x, int32_t y, int32_t flip_x, int32_t flip_y,
    int32_t width, int32_t height, uint32_t adjustment,
    uint32_t token, bool reactive) {
	struct np_window_message message;
	np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST,
	                       NP_GUEST_POPUP_PLACEMENT_REQUESTED);
	np_window_put_u32(&message, window);
	np_window_put_u32(&message, parent);
	np_window_put_i32(&message, x);
	np_window_put_i32(&message, y);
	np_window_put_i32(&message, flip_x);
	np_window_put_i32(&message, flip_y);
	np_window_put_i32(&message, width);
	np_window_put_i32(&message, height);
	np_window_put_u32(&message, adjustment);
	np_window_put_u32(&message, token);
	np_window_put_bool(&message, reactive);
	bool sent = message.ok && np_host_send_binary(
		&server->host, message.data, message.len);
	np_window_message_clear(&message);
	return sent;
}

bool np_window_event_send_frame(struct np_server *server, uint32_t surface,
                                const struct np_window_frame *frame,
                                unsigned char **bytes, size_t *size) {
	if (bytes) *bytes = NULL;
	if (size) *size = 0;
	if (!server || !frame || !bytes || !size) return false;
	struct np_window_message message;
	np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST,
	                       NP_GUEST_COMMITTED);
	np_window_put_u32(&message, surface);
	np_window_put_frame(&message, frame);
	if (!message.ok) {
		np_window_message_clear(&message);
		return false;
	}
	*bytes = message.data;
	*size = message.len;
	message.data = NULL;
	np_window_message_clear(&message);
	return true;
}
