#ifndef NATIVEPIPE_WINDOWWIRE_H
#define NATIVEPIPE_WINDOWWIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NP_WINDOW_MAX_FIELD (8u * 1024u * 1024u)
#define NP_WINDOW_MAX_COLLECTION 4096u
#define NP_WINDOW_PROTOCOL_VERSION 9u

enum np_window_direction {
	NP_WINDOW_GUEST_TO_HOST = 1,
	NP_WINDOW_HOST_TO_GUEST = 2,
};

enum np_window_guest_opcode {
	NP_GUEST_SESSION_STARTED = 1,
	NP_GUEST_SURFACE_CREATED,
	NP_GUEST_SURFACE_DESTROYED,
	NP_GUEST_SURFACE_UNMAPPED,
	NP_GUEST_TOPLEVEL_CREATED,
	NP_GUEST_TOPLEVEL_DESTROYED,
	NP_GUEST_POPUP_CREATED,
	NP_GUEST_POPUP_REPOSITIONED,
	NP_GUEST_POPUP_DESTROYED,
	NP_GUEST_SUBSURFACE_CREATED,
	NP_GUEST_SUBSURFACE_MOVED,
	NP_GUEST_SUBSURFACE_DESTROYED,
	NP_GUEST_DRAG_ICON_CHANGED,
	NP_GUEST_CURSOR_CHANGED,
	NP_GUEST_CURSOR_SHAPE_CHANGED,
	NP_GUEST_TITLE_CHANGED,
	NP_GUEST_APP_ID_CHANGED,
	NP_GUEST_DECORATION_MODE_CHANGED,
	NP_GUEST_PARENT_CHANGED,
	NP_GUEST_SIZE_CONSTRAINTS_CHANGED,
	NP_GUEST_COMMITTED,
	NP_GUEST_FRAME_CALLBACK_REQUESTED,
	NP_GUEST_INTERACTIVE_MOVE_REQUESTED,
	NP_GUEST_INTERACTIVE_RESIZE_REQUESTED,
	NP_GUEST_FULLSCREEN_REQUESTED,
	NP_GUEST_MAXIMIZE_REQUESTED,
	NP_GUEST_MINIMIZE_REQUESTED,
	NP_GUEST_SELECTION_OFFERED,
	NP_GUEST_SELECTION_DATA,
	NP_GUEST_HOST_SELECTION_REQUEST,
	NP_GUEST_TEXT_INPUT_ENABLED,
	NP_GUEST_TEXT_INPUT_CURSOR_RECT,
	NP_GUEST_TEXT_INPUT_SURROUNDING_TEXT,
	NP_GUEST_REGISTER_COMPOSITOR,
	NP_GUEST_POPUP_PLACEMENT_REQUESTED = 35,
	NP_GUEST_FORCE_QUIT_CAPABILITY_CHANGED = 36,
	NP_GUEST_FILE_DRAG = 37,
};

enum np_window_host_opcode {
	NP_HOST_CONFIGURE = 1,
	NP_HOST_CLOSE,
	NP_HOST_DISMISS_POPUP,
	NP_HOST_OUTPUT_SCALE,
	NP_HOST_KEYBOARD_FOCUS,
	NP_HOST_KEY,
	NP_HOST_POINTER_ENTERED,
	NP_HOST_POINTER_MOVED,
	NP_HOST_POINTER_LEFT,
	NP_HOST_POINTER_BUTTON,
	NP_HOST_POINTER_SCROLL,
	NP_HOST_FRAME_PRESENTED,
	NP_HOST_DISPLAY_REFRESH,
	NP_HOST_SELECTION_REQUEST,
	NP_HOST_SELECTION_OFFERED,
	NP_HOST_SELECTION_DATA,
	NP_HOST_TEXT_COMMIT,
	NP_HOST_TEXT_PREEDIT,
	NP_HOST_TEXT_DELETE_SURROUNDING,
	NP_HOST_FRAME_RELEASED,
	NP_HOST_FORCE_QUIT,
	NP_HOST_CONFIGURE_POPUP,
	NP_HOST_OUTPUTS_CHANGED,
	NP_HOST_WINDOW_OUTPUT_CHANGED,
	NP_HOST_INPUT_PREFERENCES,
	NP_HOST_CAPTURE_FRAME,
	NP_HOST_APPLICATION_REQUEST = 27,
	NP_HOST_FILE_DRAG = 28,
};

enum np_window_pixel_format {
	NP_WINDOW_BGRA8888 = 1,
	NP_WINDOW_BGRX8888 = 2,
	NP_WINDOW_RGBA8888 = 3,
};

enum np_window_frame_source {
	NP_WINDOW_FRAME_CPU = 1,
	NP_WINDOW_FRAME_GPU = 2,
	NP_WINDOW_FRAME_ENCODED = 3,
};

struct np_window_message {
	unsigned char *data;
	size_t len;
	size_t cap;
	bool ok;
};

struct np_window_reader {
	const unsigned char *data;
	size_t len;
	size_t offset;
	uint8_t opcode;
	bool ok;
};

struct np_window_rect {
	int32_t x, y, width, height;
};

struct np_window_float_rect {
	double x, y, width, height;
};

struct np_window_frame {
	uint32_t resource_id;
	int32_t width, height, bytes_per_row, scale;
	uint8_t format;
	uint8_t source;
	uint16_t bitstream_epoch;
	uint32_t presentation_id;
	bool has_viewport_source;
	struct np_window_float_rect viewport_source;
	bool has_viewport_destination;
	int32_t viewport_width, viewport_height;
	bool has_window_geometry;
	struct np_window_rect window_geometry;
	const char *codec;
	uint32_t gpu_source_id;
	bool has_gpu_source;
	const struct np_window_rect *damage;
	uint32_t damage_count;
};

void np_window_message_init(struct np_window_message *message,
	                         uint8_t direction, uint8_t opcode);
void np_window_message_clear(struct np_window_message *message);
void np_window_put_u8(struct np_window_message *message, uint8_t value);
void np_window_put_bool(struct np_window_message *message, bool value);
void np_window_put_u16(struct np_window_message *message, uint16_t value);
void np_window_put_u32(struct np_window_message *message, uint32_t value);
void np_window_put_i32(struct np_window_message *message, int32_t value);
void np_window_put_f64(struct np_window_message *message, double value);
void np_window_put_string(struct np_window_message *message, const char *value);
void np_window_put_bytes(struct np_window_message *message,
	                      const unsigned char *bytes, size_t length);
void np_window_put_optional_bytes(struct np_window_message *message,
	                               const unsigned char *bytes, size_t length,
	                               bool present);
void np_window_put_frame(struct np_window_message *message,
	                      const struct np_window_frame *frame);

bool np_window_reader_init(struct np_window_reader *reader,
	                       const unsigned char *data, size_t length,
	                       uint8_t expected_direction);
uint8_t np_window_read_u8(struct np_window_reader *reader);
bool np_window_read_bool(struct np_window_reader *reader);
uint16_t np_window_read_u16(struct np_window_reader *reader);
uint32_t np_window_read_u32(struct np_window_reader *reader);
int32_t np_window_read_i32(struct np_window_reader *reader);
double np_window_read_fixed(struct np_window_reader *reader);
double np_window_read_f64(struct np_window_reader *reader);
char *np_window_read_string(struct np_window_reader *reader);
bool np_window_read_bytes(struct np_window_reader *reader,
	                      const unsigned char **bytes, size_t *length,
	                      bool optional, bool *present);
bool np_window_reader_finished(const struct np_window_reader *reader);

#endif
