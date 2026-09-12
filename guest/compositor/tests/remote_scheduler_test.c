/* Exercise the real remote scheduler with manually completed encoders. No
 * timing-dependent slow CPU or network is needed to prove window isolation. */
#include "../backends/remote/presentation.c"
#include <assert.h>

struct np_encoder { np_encoder_done_fn done; void *user; uint8_t *pixels; uint32_t id; };
static unsigned sent[8], sent_count;
struct np_encoder *np_encoder_create(int w, int h, np_encoder_output_fn out, np_encoder_done_fn done, void *user)
{ (void)w; (void)h; (void)out; struct np_encoder *e = calloc(1, sizeof(*e)); e->done = done; e->user = user; return e; }
bool np_encoder_take_bgra(struct np_encoder *e, uint8_t *pixels, int w, int h, int stride, uint64_t pts, uint32_t id, uint8_t flags)
{ (void)w; (void)h; (void)stride; (void)pts; (void)flags; assert(!e->pixels); e->pixels = pixels; e->id = id; return true; }
void np_encoder_destroy(struct np_encoder *e) { assert(!e->pixels); free(e); }
uint16_t np_encoder_epoch(const struct np_encoder *e) { (void)e; return 1; }
static void finish(struct np_surface *s)
{ struct np_encoder *e = remote_surface(s, false)->encoder; free(e->pixels); e->pixels = NULL; e->done(e->user, true); }
void np_media_wake(struct np_media *m) { (void)m; }
bool np_media_can_encode(struct np_media *m) { (void)m; return true; }
bool np_media_send_display(struct np_media *m, const void *p, size_t n)
{ (void)m; assert(n == 164); sent[sent_count++] = get32((const unsigned char *)p + 12); return true; }
bool np_media_send(struct np_media *m, uint8_t codec, uint8_t flags, uint32_t sid, uint32_t rid, uint16_t w, uint16_t h, uint64_t pts, uint16_t epoch, const uint8_t *p, uint32_t n)
{ (void)m; (void)codec; (void)flags; (void)sid; (void)rid; (void)w; (void)h; (void)pts; (void)epoch; (void)p; (void)n; return true; }
struct np_surface *np_surface_by_id(struct np_server *server, uint32_t id)
{ struct np_surface *s; wl_list_for_each(s, &server->surfaces, link) if (s->id == id) return s; return NULL; }
int32_t np_scale_surface_refresh_millihz(const struct np_surface *s) { (void)s; return 60000; }
void np_xdg_flush_pending_toplevel_configure(struct np_surface *s) { (void)s; }
void np_presentation_process_presented(struct np_server *s, uint32_t owner, uint32_t id) { (void)s; (void)owner; (void)id; }
void np_presentation_flush(struct np_server *s) { (void)s; }
static void put(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static bool submit(struct np_server *server, struct np_surface *owner, struct np_surface *source)
{
    unsigned char p[164] = {0}; memcpy(p, "NPSN", 4);
    put(p + 12, owner->id); put(p + 16, source->last_resource_id); put(p + 48, 1);
    put(p + 76, source->id); put(p + 80, source->last_resource_id);
    return np_remote_submit_scene(server, p, sizeof(p));
}
int main(void)
{
    struct np_remote_backend backend = {0};
    struct np_server server = { .backend_state = &backend, .display = wl_display_create() };
    assert(server.display); wl_list_init(&server.surfaces);
    struct np_surface a = { .server = &server, .id = 1 }, b = { .server = &server, .id = 2 }, c = { .server = &server, .id = 3 };
    wl_list_insert(&server.surfaces, &a.link); wl_list_insert(&server.surfaces, &b.link); wl_list_insert(&server.surfaces, &c.link);
    unsigned char pixels[64] = {0};
    assert(encode_remote_pixels(&a, pixels, 4, 4, 16, WL_SHM_FORMAT_XRGB8888));
    assert(encode_remote_pixels(&b, pixels, 4, 4, 16, WL_SHM_FORMAT_XRGB8888));
    assert(encode_remote_pixels(&c, pixels, 4, 4, 16, WL_SHM_FORMAT_XRGB8888));
    uint32_t first = a.last_resource_id;
    unsigned char *owned = remote_surface(&a, false)->pixels;
    assert(submit(&server, &a, &a)); np_remote_flush_encoded(&server);
    assert(remote_surface(&a, false)->encoder->pixels == owned); // No second BGRA copy.
    assert(encode_remote_pixels(&a, pixels, 4, 4, 16, WL_SHM_FORMAT_XRGB8888));
    assert(remote_surface(&a, false)->encoder->id == first); // Immutable snapshot.
    assert(!submit(&server, &b, &a)); // Reserve each source's codec chain.
    assert(submit(&server, &b, &b)); np_remote_flush_encoded(&server);
    assert(!submit(&server, &c, &c)); // Two jobs is a hard admission bound.
    finish(&b); np_remote_flush_encoded(&server);
    assert(sent_count == 1 && sent[0] == b.id && remote_surface(&a, false)->job);
    finish(&a); np_remote_flush_encoded(&server);
    assert(sent_count == 2 && sent[1] == a.id);
    struct np_remote_surface *as = remote_surface(&a, false), *bs = remote_surface(&b, false);
    as->next_refresh_ns = monotonic_ns() + 100000000; bs->next_refresh_ns = 0;
    assert(!np_remote_scene_available(&a) && np_remote_scene_available(&b));
    as->next_refresh_ns = 0;
    owned = as->pixels;
    assert(submit(&server, &a, &a));
    np_remote_cancel_scenes(&server, a.id); np_remote_flush_encoded(&server);
    assert(as->pixels == owned && !as->job && sent_count == 2); // Unencoded rollback.
    as->next_refresh_ns = 0; // Advance the simulated clock past admission pacing.
    assert(submit(&server, &a, &a)); np_remote_flush_encoded(&server);
    np_remote_cancel_scenes(&server, a.id); // Cancelling active work waits for done.
    assert(as->job); finish(&a); np_remote_flush_encoded(&server);
    assert(!as->job && sent_count == 2);
    np_backend_surface_destroy(&a); np_backend_surface_destroy(&b); np_backend_surface_destroy(&c);
    np_remote_finish_scenes(&server); wl_display_destroy(server.display);
    puts("Independent windows, bounded jobs, immutable owned pixels, source ordering, pacing and cancellation: PASS");
}
