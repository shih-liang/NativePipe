#define _GNU_SOURCE
#include "activation.h"
#include "compositor_internal.h"
#include "scene.h"
#include "windowwire.h"
#include "xdg-activation-v1-server-protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <wayland-server-protocol.h>

enum { NP_ACTIVATION_INPUTS = 32, NP_ACTIVATION_TOKENS = 64 };

struct np_activation_input {
	struct wl_resource *surface, *origin;
	struct wl_listener surface_destroy, origin_destroy;
	uint32_t serial;
	uint64_t time_ms;
	bool claimed;
};

struct np_activation_grant {
	struct wl_list link;
	struct wl_resource *surface, *origin;
	struct wl_listener surface_destroy, origin_destroy;
	char value[33];
	char *app_id;
	uint64_t input_time_ms;
};

struct np_activation_token {
	struct wl_list link;
	struct wl_resource *resource, *seat, *surface;
	struct wl_listener seat_destroy, surface_destroy;
	struct np_activation *manager;
	uint32_t serial;
	char *app_id;
	bool committed, surface_was_set, invalid;
};

struct np_activation {
	struct np_server *server;
	struct wl_global *global;
	struct wl_listener display_destroy;
	struct wl_list grants, tokens;
	struct np_activation_input inputs[NP_ACTIVATION_INPUTS];
	uint32_t next_input;
};

static uint64_t activation_now_ms(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static bool recent(uint64_t now, uint64_t then)
{
	return now && then && now >= then && now - then <= NP_ACTIVATION_TIMEOUT_MS;
}

/* Non-key popup panels inherit their parent toplevel's activation origin. */
static struct np_surface *activation_origin(struct np_surface *surface)
{
	struct np_surface *root = surface ? np_scene_root(surface) : NULL;
	for (unsigned depth = 0; root && root->popup && depth < 64; depth++)
		root = np_surface_by_window(root->server, root->popup_parent_window);
	return root && root->toplevel && root->mapped && root->window_id ? root : NULL;
}

static void input_clear(struct np_activation_input *input)
{
	if (input->surface) wl_list_remove(&input->surface_destroy.link);
	if (input->origin) wl_list_remove(&input->origin_destroy.link);
	input->surface = input->origin = NULL;
	input->serial = 0;
}

static void input_surface_destroyed(struct wl_listener *listener, void *data)
{
	struct np_activation_input *input = wl_container_of(listener, input, surface_destroy);
	input_clear(input);
}

static void input_origin_destroyed(struct wl_listener *listener, void *data)
{
	struct np_activation_input *input = wl_container_of(listener, input, origin_destroy);
	input_clear(input);
}

static void grant_destroy(struct np_activation_grant *grant)
{
	wl_list_remove(&grant->link);
	wl_list_remove(&grant->surface_destroy.link);
	wl_list_remove(&grant->origin_destroy.link);
	free(grant->app_id);
	free(grant);
}

static void grant_surface_destroyed(struct wl_listener *listener, void *data)
{
	struct np_activation_grant *grant = wl_container_of(listener, grant, surface_destroy);
	grant_destroy(grant);
}

static void grant_origin_destroyed(struct wl_listener *listener, void *data)
{
	struct np_activation_grant *grant = wl_container_of(listener, grant, origin_destroy);
	grant_destroy(grant);
}

static void prune_grants(struct np_activation *manager, uint64_t now)
{
	struct np_activation_grant *grant, *temporary;
	wl_list_for_each_safe(grant, temporary, &manager->grants, link)
		if (!recent(now, grant->input_time_ms)) grant_destroy(grant);
}

void np_activation_revoke_surface(struct np_surface *surface)
{
	struct np_activation *manager = surface ? surface->server->activation : NULL;
	if (!manager) return;
	for (unsigned i = 0; i < NP_ACTIVATION_INPUTS; i++) {
		struct np_activation_input *input = &manager->inputs[i];
		if (input->surface == surface->resource || input->origin == surface->resource) input_clear(input);
	}
	struct np_activation_grant *grant, *temporary;
	wl_list_for_each_safe(grant, temporary, &manager->grants, link)
		if (grant->surface == surface->resource || grant->origin == surface->resource) grant_destroy(grant);
}

void np_activation_record_input(struct np_server *server,
	                           struct np_surface *surface, uint32_t serial)
{
	struct np_activation *manager = server->activation;
	struct np_surface *origin = activation_origin(surface);
	if (!manager || !serial || !origin || !surface->resource ||
	    wl_resource_get_client(surface->resource) != wl_resource_get_client(origin->resource)) return;
	uint64_t now = activation_now_ms();
	if (!now) return;
	prune_grants(manager, now);
	struct np_activation_input *input = &manager->inputs[manager->next_input++ % NP_ACTIVATION_INPUTS];
	input_clear(input);
	input->surface = surface->resource;
	input->origin = origin->resource;
	input->serial = serial;
	input->time_ms = now;
	input->claimed = false;
	input->surface_destroy.notify = input_surface_destroyed;
	input->origin_destroy.notify = input_origin_destroyed;
	wl_resource_add_destroy_listener(input->surface, &input->surface_destroy);
	wl_resource_add_destroy_listener(input->origin, &input->origin_destroy);
}

static bool token_mutable(struct np_activation_token *token)
{
	if (!token->committed) return true;
	wl_resource_post_error(token->resource, XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
	                       "activation token has already been committed");
	return false;
}

static void token_seat_destroyed(struct wl_listener *listener, void *data)
{
	struct np_activation_token *token = wl_container_of(listener, token, seat_destroy);
	wl_list_remove(&token->seat_destroy.link);
	token->seat = NULL;
}

static void token_surface_destroyed(struct wl_listener *listener, void *data)
{
	struct np_activation_token *token = wl_container_of(listener, token, surface_destroy);
	wl_list_remove(&token->surface_destroy.link);
	token->surface = NULL;
}

static void token_set_serial(struct wl_client *client, struct wl_resource *resource,
	                         uint32_t serial, struct wl_resource *seat)
{
	struct np_activation_token *token = wl_resource_get_user_data(resource);
	if (!token_mutable(token)) return;
	if (token->seat) wl_list_remove(&token->seat_destroy.link);
	token->serial = serial;
	token->seat = seat;
	token->seat_destroy.notify = token_seat_destroyed;
	wl_resource_add_destroy_listener(seat, &token->seat_destroy);
}

static void token_set_surface(struct wl_client *client, struct wl_resource *resource,
	                          struct wl_resource *surface)
{
	struct np_activation_token *token = wl_resource_get_user_data(resource);
	if (!token_mutable(token)) return;
	if (token->surface) wl_list_remove(&token->surface_destroy.link);
	token->surface = surface;
	token->surface_was_set = true;
	token->surface_destroy.notify = token_surface_destroyed;
	wl_resource_add_destroy_listener(surface, &token->surface_destroy);
}

static void token_set_app_id(struct wl_client *client, struct wl_resource *resource,
	                         const char *app_id)
{
	struct np_activation_token *token = wl_resource_get_user_data(resource);
	if (!token_mutable(token)) return;
	if (strlen(app_id) > 512) { token->invalid = true; return; }
	char *copy = strdup(app_id);
	if (!copy) { wl_client_post_no_memory(client); return; }
	free(token->app_id);
	token->app_id = copy;
}

static bool random_token(char value[33])
{
	unsigned char bytes[16];
	size_t offset = 0;
	while (offset < sizeof(bytes)) {
		ssize_t count = getrandom(bytes + offset, sizeof(bytes) - offset, 0);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return false;
		offset += (size_t)count;
	}
	static const char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < sizeof(bytes); i++) {
		value[2 * i] = hex[bytes[i] >> 4];
		value[2 * i + 1] = hex[bytes[i] & 15];
	}
	value[32] = 0;
	return true;
}

static struct np_activation_input *token_input(struct np_activation_token *token,
	                                          struct wl_client *client, uint64_t now)
{
	struct np_server *server = token->manager->server;
	if (token->invalid || !token->seat ||
	    wl_resource_get_client(token->seat) != client ||
	    strcmp(wl_resource_get_class(token->seat), "wl_seat") ||
	    wl_resource_get_user_data(token->seat) != server ||
	    (token->surface_was_set && !token->surface)) return NULL;
	if (token->surface && wl_resource_get_client(token->surface) != client) return NULL;
	struct np_surface *focused = activation_origin(np_surface_by_window(server, server->focused_window));
	if (!focused || wl_resource_get_client(focused->resource) != client) return NULL;
	for (unsigned i = 0; i < NP_ACTIVATION_INPUTS; i++) {
		struct np_activation_input *input = &token->manager->inputs[i];
		if (!input->surface || !input->origin || input->claimed ||
		    input->serial != token->serial || !recent(now, input->time_ms) ||
		    input->origin != focused->resource ||
		    wl_resource_get_client(input->surface) != client) continue;
		if (token->surface && activation_origin(wl_resource_get_user_data(token->surface)) != focused) continue;
		return input;
	}
	return NULL;
}

static void token_commit(struct wl_client *client, struct wl_resource *resource)
{
	struct np_activation_token *token = wl_resource_get_user_data(resource);
	if (!token_mutable(token)) return;
	token->committed = true;
	uint64_t now = activation_now_ms();
	prune_grants(token->manager, now);
	struct np_activation_input *input = token_input(token, client, now);
	if (!input || wl_list_length(&token->manager->grants) >= NP_ACTIVATION_TOKENS) {
		xdg_activation_token_v1_send_done(resource, "");
		return;
	}
	struct np_activation_grant *grant = calloc(1, sizeof(*grant));
	if (!grant) { wl_client_post_no_memory(client); return; }
	if (!random_token(grant->value)) {
		free(grant);
		xdg_activation_token_v1_send_done(resource, "");
		return;
	}
	grant->app_id = token->app_id ? strdup(token->app_id) : NULL;
	if (token->app_id && !grant->app_id) {
		free(grant); wl_client_post_no_memory(client); return;
	}
	input->claimed = true;
	grant->surface = token->surface ? token->surface : input->surface;
	grant->origin = input->origin;
	grant->input_time_ms = input->time_ms;
	grant->surface_destroy.notify = grant_surface_destroyed;
	grant->origin_destroy.notify = grant_origin_destroyed;
	wl_resource_add_destroy_listener(grant->surface, &grant->surface_destroy);
	wl_resource_add_destroy_listener(grant->origin, &grant->origin_destroy);
	wl_list_insert(token->manager->grants.prev, &grant->link);
	xdg_activation_token_v1_send_done(resource, grant->value);
}

static void token_destroy_resource(struct wl_resource *resource)
{
	struct np_activation_token *token = wl_resource_get_user_data(resource);
	if (token->seat) wl_list_remove(&token->seat_destroy.link);
	if (token->surface) wl_list_remove(&token->surface_destroy.link);
	wl_list_remove(&token->link);
	free(token->app_id);
	free(token);
}

static void destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct xdg_activation_token_v1_interface token_implementation = {
	.set_serial = token_set_serial,
	.set_app_id = token_set_app_id,
	.set_surface = token_set_surface,
	.commit = token_commit,
	.destroy = destroy_request,
};

static void get_activation_token(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct np_activation *manager = wl_resource_get_user_data(resource);
	struct np_activation_token *token = calloc(1, sizeof(*token));
	if (!token) { wl_client_post_no_memory(client); return; }
	token->resource = wl_resource_create(client, &xdg_activation_token_v1_interface, 1, id);
	if (!token->resource) { free(token); wl_client_post_no_memory(client); return; }
	token->manager = manager;
	wl_list_insert(manager->tokens.prev, &token->link);
	wl_resource_set_implementation(token->resource, &token_implementation, token, token_destroy_resource);
}

static bool app_id_matches(const char *expected, const char *actual)
{
	if (!expected || !expected[0]) return true;
	if (!actual) return false;
	size_t count = strlen(expected);
	if (count > 8 && !strcmp(expected + count - 8, ".desktop")) count -= 8;
	size_t actual_count = strlen(actual);
	if (actual_count > 8 && !strcmp(actual + actual_count - 8, ".desktop")) actual_count -= 8;
	return actual_count == count && !strncmp(expected, actual, count);
}

static void activate_request(struct wl_client *client, struct wl_resource *resource,
	                         const char *value, struct wl_resource *target_resource)
{
	struct np_activation *manager = wl_resource_get_user_data(resource);
	uint64_t now = activation_now_ms();
	prune_grants(manager, now);
	struct np_activation_grant *grant, *temporary;
	wl_list_for_each_safe(grant, temporary, &manager->grants, link) {
		if (strcmp(value, grant->value)) continue;
		struct np_surface *origin = wl_resource_get_user_data(grant->origin);
		struct np_surface *focused = activation_origin(np_surface_by_window(manager->server, manager->server->focused_window));
		struct np_surface *target = wl_resource_get_user_data(target_resource);
		bool valid = origin && focused == origin && activation_origin(origin) == origin &&
			target && target->resource == target_resource && target->server == manager->server &&
			wl_resource_get_client(target_resource) == client &&
			target->toplevel && target->mapped && target->window_id &&
			activation_origin(wl_resource_get_user_data(grant->surface)) == origin &&
			app_id_matches(grant->app_id, target->app_id);
		uint32_t window = valid ? target->window_id : 0;
		uint32_t origin_window = valid ? origin->window_id : 0;
		uint32_t age = (uint32_t)(now - grant->input_time_ms);
		/* An unsuccessful use also consumes the token, preventing replay when
		 * focus returns or against a different target. */
		grant_destroy(grant);
		if (!valid) return;
		struct np_window_message message;
		np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST, NP_GUEST_ACTIVATION_REQUESTED);
		np_window_put_u32(&message, window);
		np_window_put_u32(&message, origin_window);
		np_window_put_u32(&message, age);
		if (message.ok) np_backend_send_binary(manager->server, message.data, message.len);
		np_window_message_clear(&message);
		return;
	}
}

static const struct xdg_activation_v1_interface activation_implementation = {
	.destroy = destroy_request,
	.get_activation_token = get_activation_token,
	.activate = activate_request,
};

static void activation_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client, &xdg_activation_v1_interface, 1, id);
	if (!resource) { wl_client_post_no_memory(client); return; }
	wl_resource_set_implementation(resource, &activation_implementation, data, NULL);
}

static void activation_display_destroyed(struct wl_listener *listener, void *data)
{
	struct np_activation *manager = wl_container_of(listener, manager, display_destroy);
	struct np_activation_token *token, *token_temporary;
	wl_list_for_each_safe(token, token_temporary, &manager->tokens, link) wl_resource_destroy(token->resource);
	struct np_activation_grant *grant, *grant_temporary;
	wl_list_for_each_safe(grant, grant_temporary, &manager->grants, link) grant_destroy(grant);
	for (unsigned i = 0; i < NP_ACTIVATION_INPUTS; i++) input_clear(&manager->inputs[i]);
	wl_list_remove(&manager->display_destroy.link);
	manager->server->activation = NULL;
	free(manager);
}

bool np_activation_advertise(struct np_server *server)
{
	if (server->activation) return true;
	struct np_activation *manager = calloc(1, sizeof(*manager));
	if (!manager) return false;
	manager->server = server;
	wl_list_init(&manager->grants);
	wl_list_init(&manager->tokens);
	manager->global = wl_global_create(server->display, &xdg_activation_v1_interface, 1, manager, activation_bind);
	if (!manager->global) { free(manager); return false; }
	server->activation = manager;
	manager->display_destroy.notify = activation_display_destroyed;
	wl_display_add_destroy_listener(server->display, &manager->display_destroy);
	return true;
}
