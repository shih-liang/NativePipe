/* Bounded component benchmark: production BGRA -> AV1 + alpha worker.
 * Pixels are a deterministic moving UI-like pattern, not a displayed GTK app.
 * Usage: remote-encoder-bench WIDTH HEIGHT [FRAMES [PACKETS_FILE]]
 * Optional output uses the same length-prefixed packet fixture as AV1 tests. */
#include "../../encoder/encoder.h"
#include <dav1d/dav1d.h>
#include <libyuv/convert_argb.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct result {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    bool done;
    double start, elapsed;
    size_t bytes;
    FILE *packets;
    Dav1dContext *decoder;
    const uint8_t *original;
    double squared_error;
};
static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void output(void *user, const uint8_t *packet, size_t size, uint64_t pts,
    uint32_t id, uint8_t flags, const uint8_t *alpha, uint32_t alpha_size,
    uint16_t epoch, uint16_t width, uint16_t height, enum np_encoder_codec codec) {
    (void)pts; (void)id; (void)flags; (void)alpha; (void)epoch; (void)width; (void)height;
    struct result *r = user; assert(codec == NP_ENCODER_AV1);
    r->elapsed = (now() - r->start) * 1000;
    r->bytes = size + alpha_size;
    // Independent decoding and error measurement are outside the encoder timer.
    Dav1dData data = {0};
    uint8_t *copy = dav1d_data_create(&data, size); assert(copy);
    memcpy(copy, packet, size);
    assert(dav1d_send_data(r->decoder, &data) == 0); dav1d_data_unref(&data);
    Dav1dPicture picture = {0}; assert(dav1d_get_picture(r->decoder, &picture) == 0);
    assert(picture.p.w == width && picture.p.h == height && picture.p.bpc == 8);
    uint8_t *decoded = malloc((size_t)width * height * 4); assert(decoded);
    assert(I420ToARGB(picture.data[0], picture.stride[0], picture.data[1], picture.stride[1],
        picture.data[2], picture.stride[1], decoded, width * 4, width, height) == 0);
    if (id > 10) for (size_t p = 0; p < (size_t)width * height; p++) {
        for (int c = 0; c < 3; c++) {
            int difference = (int)decoded[p * 4 + c] - r->original[p * 4 + c];
            r->squared_error += difference * difference;
        }
    }
    free(decoded); dav1d_picture_unref(&picture);
    if (r->packets) {
        assert(size <= UINT32_MAX);
        uint8_t length[4] = {size, size >> 8, size >> 16, size >> 24};
        assert(fwrite(length, 1, 4, r->packets) == 4);
        assert(fwrite(packet, 1, size, r->packets) == size);
    }
}
static void done(void *user, bool ok) {
    struct result *r = user; assert(ok);
    pthread_mutex_lock(&r->lock); r->done = true;
    pthread_cond_signal(&r->ready); pthread_mutex_unlock(&r->lock);
}
static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
int main(int argc, char **argv) {
    assert(argc >= 3 && argc <= 5);
    int w = atoi(argv[1]), h = atoi(argv[2]), frames = argc >= 4 ? atoi(argv[3]) : 90;
    assert(w > 0 && h > 0 && w <= 4096 && h <= 4096 && !(w % 2) && !(h % 2));
    assert(frames >= 20 && frames <= 600);
    struct result r = {.lock=PTHREAD_MUTEX_INITIALIZER, .ready=PTHREAD_COND_INITIALIZER};
    Dav1dSettings settings; dav1d_default_settings(&settings);
    settings.n_threads = settings.max_frame_delay = 1; settings.strict_std_compliance = 1;
    assert(dav1d_open(&r.decoder, &settings) == 0);
    if (argc == 5) { r.packets = fopen(argv[4], "wb"); assert(r.packets); }
    struct np_encoder *encoder = np_encoder_create(w, h, false, output, done, &r); assert(encoder);
    double *samples = calloc(frames, sizeof(double)), total = 0, first = 0;
    size_t bytes = 0; int count = 0; assert(samples);
    for (int frame = 0; frame < frames; frame++) {
        uint8_t *pixels = malloc((size_t)w * h * 4); assert(pixels);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint8_t *p = pixels + ((size_t)y * w + x) * 4;
            int bar = (y / 43) % 12, moving = (x + frame * 7 + bar * 71) % 240 < 80;
            p[0] = moving ? 220 - bar * 9 : 30;
            p[1] = moving ? 180 : 18; p[2] = moving ? 40 + bar * 10 : 10;
            if (y % 80 > 60 && x % 21 < 2) p[0] = p[1] = p[2] = 220;
            p[3] = x < 8 && y < 8 ? 0 : 255;
        }
        r.original = pixels; r.start = now();
        assert(np_encoder_take_bgra(encoder, pixels, w, h, w * 4,
            (uint64_t)frame * 16666667, frame + 1, NP_ENCODER_FLAG_HAS_ALPHA));
        pthread_mutex_lock(&r.lock);
        struct timespec limit; clock_gettime(CLOCK_REALTIME, &limit); limit.tv_sec += 10;
        while (!r.done) assert(pthread_cond_timedwait(&r.ready, &r.lock, &limit) == 0);
        r.done = false;
        if (!frame) first = r.elapsed;
        if (frame >= 10) { samples[count++] = r.elapsed; total += r.elapsed; bytes += r.bytes; }
        pthread_mutex_unlock(&r.lock);
    }
    np_encoder_destroy(encoder);
    qsort(samples, count, sizeof(double), compare);
    double mse = r.squared_error / ((double)count * w * h * 3);
    printf("{\"width\":%d,\"height\":%d,\"samples\":%d,\"first_ms\":%.3f,\"mean_ms\":%.3f,\"p50_ms\":%.3f,\"p95_ms\":%.3f,\"max_ms\":%.3f,\"mean_bytes\":%zu,\"rgb_psnr_db\":%.3f}\n",
        w, h, count, first, total / count, samples[count / 2], samples[(count * 95 - 1) / 100], samples[count - 1], bytes / count,
        10 * log10(255.0 * 255.0 / fmax(mse, 0.000001)));
    if (r.packets) assert(fclose(r.packets) == 0);
    dav1d_close(&r.decoder); free(samples); pthread_mutex_destroy(&r.lock); pthread_cond_destroy(&r.ready);
}
