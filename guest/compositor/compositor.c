// Shared RemotePipe/VMPipe Wayland compositor assembly.
//
// Protocol state, input, presentation and host transport live in focused
// translation units. This file owns only process setup and the event loop.

#define _GNU_SOURCE

#include "compositor.h"
#include "compositor_internal.h"
#include "cursor_shape.h"
#include "data_device.h"
#include "decoration.h"
#include "fifo.h"
#include "scale.h"
#include "text_input.h"
#include "xdg_shell.h"
#include "xwayland.h"
#include "fifo-v1-server-protocol.h"
#include "text-input-v3-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

#define COMPOSITOR_VERSION 4
#define XDG_WM_BASE_VERSION 3
#define SEAT_VERSION 5

#ifndef NP_COMPOSITOR_SOURCE_HASH
#error "NP_COMPOSITOR_SOURCE_HASH must be supplied by the compositor Makefile"
#endif

/* Survives strip(1), allowing the host packager to reject a stale guest ELF. */
__attribute__((used)) static const char np_compositor_source_stamp[] =
	"NPCS:" NP_COMPOSITOR_SOURCE_HASH;

bool np_trace_enabled(void)
{
	static int enabled = -1;
	if (enabled < 0)
		enabled = getenv("NP_TRACE") != NULL ||
		          access("/run/nativepipe-trace", F_OK) == 0;
	return enabled == 1;
}

int np_frontend_run(int argc, char **argv, void *backend_state)
{
	(void)argc;
	(void)argv;

	struct np_server server;
	memset(&server, 0, sizeof(server));
	server.backend_state = backend_state;
	server.next_id = 1;
	server.output_scale = 2;
	server.output_width = 3024;
	server.output_height = 1964;
	server.keymap_fd = -1;
	memcpy(server.keyboard_layout, "us", sizeof("us"));
	server.key_repeat_rate = 25;
	server.key_repeat_delay = 600;
	wl_list_init(&server.surfaces);
	wl_list_init(&server.shm_textures);
	wl_list_init(&server.output_states);
	wl_list_init(&server.outputs);
	wl_list_init(&server.pointers);
	wl_list_init(&server.keyboards);
	wl_list_init(&server.data_devices);
	wl_list_init(&server.data_offers);
	wl_list_init(&server.clip_reads);
	wl_list_init(&server.clip_pending);
	wl_list_init(&server.clip_writes);
	wl_list_init(&server.text_inputs);

	/* A peer closing a clipboard pipe must not terminate the compositor. */
	signal(SIGPIPE, SIG_IGN);
	np_backend_session_reset_readiness();
	if (!np_backend_prepare(&server)) return 1;

	server.display = wl_display_create();
	if (!server.display) {
		fprintf(stderr, "[wayland] wl_display_create failed\n");
		return 1;
	}
	if (wl_display_init_shm(server.display) < 0) {
		fprintf(stderr, "[wayland] wl_display_init_shm failed\n");
		return 1;
	}

	wl_global_create(server.display, &wl_compositor_interface,
	                 COMPOSITOR_VERSION, &server, np_compositor_bind);
	wl_global_create(server.display, &wl_subcompositor_interface,
	                 1, &server, np_subcompositor_bind);
	wl_global_create(server.display, &wl_data_device_manager_interface,
	                 3, &server, np_data_device_manager_bind);
	wl_global_create(server.display, &xdg_wm_base_interface,
	                 XDG_WM_BASE_VERSION, &server, np_xdg_shell_bind);
	wl_global_create(server.display, &wl_seat_interface,
	                 SEAT_VERSION, &server, np_seat_bind);
	np_scale_advertise(server.display, &server);
	np_cursor_shape_advertise(server.display, &server);
	wl_global_create(server.display, &wp_fifo_manager_v1_interface,
	                 1, &server, np_fifo_manager_bind);
	wl_global_create(server.display, &zwp_text_input_manager_v3_interface,
	                 1, &server, np_text_input_manager_bind);
	np_decoration_advertise(server.display, &server);
	np_backend_advertise_globals(&server);

	/* Host endpoints exist before the Wayland socket becomes launchable. */
	if (!np_backend_session_listen(&server)) return 1;

	const char *socket = wl_display_add_socket_auto(server.display);
	if (!socket) {
		fprintf(stderr, "[wayland] could not create a Wayland socket\n");
		return 1;
	}
	fprintf(stderr, "[wayland] WAYLAND_DISPLAY=%s\n", socket);
	if (!np_backend_session_set_socket(&server, socket)) {
		fprintf(stderr, "[wayland] could not publish display name\n");
		return 1;
	}
	if (!np_xwayland_init(&server))
		fprintf(stderr, "[wayland] Xwayland integration unavailable\n");

	if (!np_input_create_keymap(&server))
		fprintf(stderr, "[wayland] no keymap; keyboard input will not work\n");

	struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
	np_backend_session_attach(&server, loop);

	for (;;) {
		np_presentation_flush(&server);
		np_backend_session_sync(&server);
		wl_display_flush_clients(server.display);
		wl_event_loop_dispatch(loop, -1);
		np_presentation_flush(&server);
		np_backend_session_sync(&server);
	}

	np_backend_session_finish(&server);
	np_xwayland_finish(&server);
	wl_display_destroy(server.display);
	np_backend_finish(&server);
	return 0;
}
