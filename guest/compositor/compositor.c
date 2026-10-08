// Shared RemotePipe/VMPipe Wayland compositor assembly.
//
// Protocol state, input, presentation and host transport live in focused
// translation units. This file owns only process setup and the event loop.

#define _GNU_SOURCE

#include "applications.h"
#include "activation.h"
#include "presentation_time.h"
#include "compositor.h"
#include "compositor_internal.h"
#include "cursor_shape.h"
#include "data_device.h"
#include "decoration.h"
#include "fifo.h"
#include "host_open.h"
#include "notifications.h"
#include "scale.h"
#include "text_input.h"
#include "xdg_shell.h"
#include "xwayland.h"
#include "fifo-v1-server-protocol.h"
#include "text-input-v3-server-protocol.h"
#include "xdg-shell-server-protocol.h"
#include "user_text.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

#define COMPOSITOR_VERSION 6
#define XDG_WM_BASE_VERSION 6
#define SEAT_VERSION 9

#ifndef NP_COMPOSITOR_SOURCE_HASH
#error "NP_COMPOSITOR_SOURCE_HASH must be supplied by the compositor Makefile"
#endif

/* Survives strip(1) and --gc-sections, allowing stale guest ELF detection. */
__attribute__((used, retain)) static const char np_compositor_source_stamp[] =
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
	/* Never publish a session which cannot accept keyboard input. */
	if (!np_input_create_keymap(&server)) return 1;
	if (!np_backend_prepare(&server)) return 1;

	server.display = wl_display_create();
	if (!server.display) {
		np_user_text("STARTUP_FAILED", "The NativePipe compositor couldn’t start (%s).", "wl_display_create failed", NULL);
		return 1;
	}
	if (wl_display_init_shm(server.display) < 0) {
		np_user_text("STARTUP_FAILED", "The NativePipe compositor couldn’t start (%s).", "wl_display_init_shm failed", NULL);
		return 1;
	}

	wl_global_create(server.display, &wl_compositor_interface,
	                 COMPOSITOR_VERSION, &server, np_compositor_bind);
	wl_global_create(server.display, &wl_subcompositor_interface,
	                 1, &server, np_subcompositor_bind);
	wl_global_create(server.display, &wl_data_device_manager_interface,
	                 4, &server, np_data_device_manager_bind);
	wl_global_create(server.display, &xdg_wm_base_interface,
	                 XDG_WM_BASE_VERSION, &server, np_xdg_shell_bind);
	wl_global_create(server.display, &wl_seat_interface,
	                 SEAT_VERSION, &server, np_seat_bind);
	if (!np_activation_advertise(&server)) return 1;
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
		np_user_text("STARTUP_FAILED", "The NativePipe compositor couldn’t start (%s).", "could not create a Wayland socket", NULL);
		return 1;
	}
	np_debug_log("[wayland] WAYLAND_DISPLAY=%s\n", socket);
	if (!np_backend_session_set_socket(&server, socket)) {
		np_user_text("STARTUP_FAILED", "The NativePipe compositor couldn’t start (%s).", "could not publish the display name", NULL);
		return 1;
	}
	if (!np_xwayland_init(&server))
		np_debug_log("[wayland] Xwayland integration unavailable\n"); /* cause already reported */

    setenv("WAYLAND_DISPLAY", server.session_socket, 1);
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    setenv("XDG_CURRENT_DESKTOP", "NativePipe", 1);
    setenv("GDK_BACKEND", "wayland", 0);
    setenv("QT_QPA_PLATFORM", "wayland", 0);
    setenv("MOZ_ENABLE_WAYLAND", "1", 0);
    unsetenv("WAYLAND_SOCKET");
    if (server.xwayland_display[0]) {
        setenv("DISPLAY", server.xwayland_display, 1);
        setenv("XAUTHORITY", server.xwayland_auth, 1);
        /* Keep a directory the user already chose (overwrite = 0). */
        if (server.xwayland_appdefaults[0])
            setenv("XAPPLRESDIR", server.xwayland_appdefaults, 0);
    } else { unsetenv("DISPLAY"); unsetenv("XAUTHORITY"); }
    bool remote_stdio = false;
    for (int i = 1; i < argc && strcmp(argv[i], "--"); i++)
        if (!strcmp(argv[i], "--stdio")) remote_stdio = true;
    if (remote_stdio && !np_host_open_init(&server)) {
        np_user_text("STARTUP_FAILED", "The NativePipe compositor couldn’t start (%s).", "cannot start the host open socket", NULL);
        np_backend_session_finish(&server);
        np_xwayland_finish(&server);
        wl_display_destroy(server.display);
        np_backend_finish(&server);
        return 1;
    }
    struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
    server.application_generation = 1;
    bool applications_started = np_applications_init(&server);
    /* Desktop notifications are optional: without a session bus there is simply
     * nothing for applications to call. */
    if (!np_notifications_init(&server))
        np_debug_log("[notifications] not started\n");
    if (!applications_started) {
        np_user_text("STARTUP_FAILED", "The NativePipe compositor couldn’t start (%s).", "cannot start the application service", NULL);
        server.terminate = true;
    } else np_backend_session_attach(&server, loop);
	while (!server.terminate) {
		np_presentation_flush(&server);
		np_backend_session_sync(&server);
		wl_display_flush_clients(server.display);
        if (server.terminate) break;
		wl_event_loop_dispatch(loop, -1);
		np_presentation_flush(&server);
		np_backend_session_sync(&server);
	}

    np_host_open_finish(&server);
    np_notifications_finish(&server);
    np_applications_finish(&server);
	wl_display_destroy_clients(server.display);
	np_backend_session_finish(&server);
	np_xwayland_finish(&server);
	np_presentation_time_destroy(&server);
	wl_display_destroy(server.display);
	np_backend_finish(&server);
	return applications_started ? 0 : 1;
}
