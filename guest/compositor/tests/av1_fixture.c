/* Generate reproducible AV1 inter-frame fixtures using the production encoder.
 * Output: eight little-endian u32 lengths followed by each temporal unit. */
#include "../../encoder/encoder.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct fixture { FILE *file; pthread_mutex_t lock; pthread_cond_t ready; bool done; };
static void output(void *user, const uint8_t *bytes, size_t size, uint64_t pts,
    uint32_t id, uint8_t flags, const uint8_t *alpha, uint32_t alpha_size,
    uint16_t epoch, uint16_t w, uint16_t h, enum np_encoder_codec codec) {
    (void)pts; (void)id; (void)flags; (void)alpha; (void)alpha_size; (void)epoch;
    struct fixture *f = user; assert(codec == NP_ENCODER_AV1);
    assert(w == 128 && h == 96 && size < UINT32_MAX);
    uint8_t length[4] = {size, size >> 8, size >> 16, size >> 24};
    assert(fwrite(length, 1, 4, f->file) == 4);
    assert(fwrite(bytes, 1, size, f->file) == size);
}
static void done(void *user, bool ok) {
    struct fixture *f = user; assert(ok);
    pthread_mutex_lock(&f->lock); f->done = true;
    pthread_cond_signal(&f->ready); pthread_mutex_unlock(&f->lock);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    struct fixture f = {.file=fopen(argv[1], "wb"), .lock=PTHREAD_MUTEX_INITIALIZER, .ready=PTHREAD_COND_INITIALIZER};
    assert(f.file);
    struct np_encoder *encoder = np_encoder_create(128, 96, false, output, done, &f);
    assert(encoder);
    for (unsigned i = 0; i < 8; i++) {
        uint8_t *pixels = malloc(128 * 96 * 4); assert(pixels);
        for (unsigned y = 0; y < 96; y++) for (unsigned x = 0; x < 128; x++) {
            uint8_t *p = pixels + (y * 128 + x) * 4;
            p[0] = x < 64 ? 240 : 0; p[1] = y * 2; p[2] = i * 30; p[3] = 255;
        }
        assert(np_encoder_take_bgra(encoder, pixels, 128, 96, 512, i, i + 1, 0));
        pthread_mutex_lock(&f.lock);
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 10;
        while (!f.done) assert(pthread_cond_timedwait(&f.ready, &f.lock, &until) == 0);
        f.done = false; pthread_mutex_unlock(&f.lock);
    }
    np_encoder_destroy(encoder);
    assert(fclose(f.file) == 0);
    pthread_mutex_destroy(&f.lock); pthread_cond_destroy(&f.ready);
}
