#include "../../encoder/encoder.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct result {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    bool done;
    unsigned calls;
    uint16_t epoch;
};
static void output(void *data, const uint8_t *bytes, size_t size, uint64_t pts,
    uint32_t id, uint8_t flags, const uint8_t *alpha, uint32_t alpha_size,
    uint16_t epoch, uint16_t width, uint16_t height)
{
    struct result *r = data;
    assert(bytes && size && pts == id);
    assert(width == (id >= 3 ? 322 : 320) && height == 242);
    assert(flags & NP_ENCODER_FLAG_HAS_ALPHA);
    if (id == 2) {
        assert(flags & NP_ENCODER_FLAG_REUSE_ALPHA);
        assert(!alpha && !alpha_size && epoch == r->epoch);
    } else {
        assert(!(flags & NP_ENCODER_FLAG_REUSE_ALPHA) && alpha && alpha_size);
        if (id == 3) assert(epoch != r->epoch);
    }
    r->epoch = epoch;
    r->calls++;
}
static void done(void *data, bool ok)
{
    struct result *r = data;
    assert(ok);
    pthread_mutex_lock(&r->lock);
    r->done = true;
    pthread_cond_signal(&r->ready);
    pthread_mutex_unlock(&r->lock);
}
int main(void)
{
    struct result r = { .lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER };
    struct np_encoder *enc = np_encoder_create(320, 242, output, done, &r);
    assert(enc);
    uint8_t *pixels = calloc(322 * 242, 4);
    assert(pixels);
    for (uint32_t id = 1; id <= 4; id++) {
        int width = id >= 3 ? 321 : 320;
        if (id == 2) pixels[0] = 100; /* RGB changes alone reuse alpha. */
        if (id == 4) pixels[3] = 128;
        assert(np_encoder_push_bgra(enc, pixels, width, 241, width * 4,
            id, id, NP_ENCODER_FLAG_HAS_ALPHA));
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 10;
        pthread_mutex_lock(&r.lock);
        while (!r.done) assert(pthread_cond_timedwait(&r.ready, &r.lock, &until) == 0);
        r.done = false;
        assert(r.calls == id);
        pthread_mutex_unlock(&r.lock);
    }
    np_encoder_destroy(enc);
    free(pixels);
    pthread_cond_destroy(&r.ready); pthread_mutex_destroy(&r.lock);
    puts("Encoder completion, alpha reuse/change, odd-size padding and resize epoch: PASS");
}
