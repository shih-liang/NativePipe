#include "../../encoder/encoder.h"
#include "../../encoder/alpha.h"
#include <dav1d/dav1d.h>
#include <libyuv/convert_argb.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct result {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    bool done;
    bool tiny;
    unsigned calls;
    uint16_t epoch;
    Dav1dContext *decoder;
};
static void output(void *data, const uint8_t *bytes, size_t size, uint64_t pts,
    uint32_t id, uint8_t flags, const uint8_t *alpha, uint32_t alpha_size,
    uint16_t epoch, uint16_t width, uint16_t height, enum np_encoder_codec codec)
{
    struct result *r = data; assert(codec == NP_ENCODER_AV1);
    assert(bytes && size && pts == id);
    assert(width == (r->tiny ? 2 : id >= 3 ? 322 : 320) && height == (r->tiny ? 2 : 242));
    // Decode the real packet using an independent implementation. A nonempty
    // encoder output alone does not prove a usable, correctly labelled frame.
    if (epoch != r->epoch) {
        if (r->decoder) dav1d_close(&r->decoder);
        Dav1dSettings settings;
        dav1d_default_settings(&settings);
        settings.n_threads = settings.max_frame_delay = 1;
        settings.strict_std_compliance = 1;
        assert(dav1d_open(&r->decoder, &settings) == 0);
    }
    Dav1dData packet = {0};
    uint8_t *copy = dav1d_data_create(&packet, size);
    assert(copy); memcpy(copy, bytes, size);
    assert(dav1d_send_data(r->decoder, &packet) == 0);
    dav1d_data_unref(&packet);
    Dav1dPicture picture = {0};
    assert(dav1d_get_picture(r->decoder, &picture) == 0);
    assert(picture.p.w == width && picture.p.h == height && picture.p.bpc == 8);
    uint8_t *decoded = malloc((size_t)width * height * 4); assert(decoded);
    assert(I420ToARGB(picture.data[0], picture.stride[0], picture.data[1], picture.stride[1],
        picture.data[2], picture.stride[1], decoded, width * 4, width, height) == 0);
    const uint8_t *center = decoded + ((height / 2) * width + width / 2) * 4;
    assert(abs(center[0] - (32 + (int)id * 25)) < 20);
    assert(abs(center[1] - 90) < 20 && abs(center[2] - 40) < 20);
    free(decoded); dav1d_picture_unref(&picture);
    assert(dav1d_get_picture(r->decoder, &picture) == DAV1D_ERR(EAGAIN));
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
    // Tight and padded rows must encode identical alpha, regardless of padding.
    uint8_t tight[7*19*4], padded[40*19];
    memset(tight, 0, sizeof(tight)); memset(padded, 211, sizeof(padded));
    for (unsigned y=0;y<19;y++) for (unsigned x=0;x<7;x++) {
        uint8_t alpha = y<9 ? 255 : (x+y)%3 == 0 ? 0 : x*17+y;
        tight[(y*7+x)*4+3] = padded[y*40+x*4+3] = alpha;
    }
    uint8_t *a, *b; uint32_t an, bn;
    assert(np_alpha_pack_bgra(tight,7,19,28,&a,&an));
    assert(np_alpha_pack_bgra(padded,7,19,40,&b,&bn));
    assert(an==bn && !memcmp(a,b,an)); free(a); free(b);
    struct result r = { .lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER };
    struct np_encoder *enc = np_encoder_create(320, 242, false, output, done, &r);
    assert(enc);
    uint8_t *pixels = calloc(322 * 242, 4);
    assert(pixels);
    for (uint32_t id = 1; id <= 4; id++) {
        int width = id >= 3 ? 321 : 320;
        for (int i = 0; i < width * 241; i++) {
            pixels[i * 4] = 32 + id * 25;
            pixels[i * 4 + 1] = 90; pixels[i * 4 + 2] = 40; pixels[i * 4 + 3] = 255;
        }
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
    dav1d_close(&r.decoder);
    free(pixels);
    // A 1x1 logical Wayland surface is edge-padded to this real 2x2 video.
    // Decode it too: accepting the compositor snapshot is only half the path.
    r.tiny = true; r.done = false; r.epoch = 0; r.calls = 0; r.decoder = NULL;
    enc = np_encoder_create(2, 2, false, output, done, &r);
    assert(enc);
    uint8_t *tiny = malloc(16); assert(tiny);
    for (unsigned i = 0; i < 4; i++) {
        tiny[i * 4] = 57; tiny[i * 4 + 1] = 90;
        tiny[i * 4 + 2] = 40; tiny[i * 4 + 3] = 255;
    }
    assert(np_encoder_take_bgra(enc, tiny, 2, 2, 8, 1, 1, NP_ENCODER_FLAG_HAS_ALPHA));
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 10;
    pthread_mutex_lock(&r.lock);
    while (!r.done) assert(pthread_cond_timedwait(&r.ready, &r.lock, &until) == 0);
    assert(r.calls == 1);
    pthread_mutex_unlock(&r.lock);
    np_encoder_destroy(enc); dav1d_close(&r.decoder);
    pthread_cond_destroy(&r.ready); pthread_mutex_destroy(&r.lock);
    puts("AV1 decoded colors/interframes, completion, alpha reuse/change, odd-size padding and resize epoch: PASS");
}
