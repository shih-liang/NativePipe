/* Opt-in real NVENC test. The NPEN fixture is decoded by the macOS tests. */
#include "../../encoder/encoder.h"
#include "../media.h"
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
    FILE *file;
    double start, elapsed;
    uint16_t epoch;
};
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void put32(uint8_t *p, uint32_t n) { for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(n>>(i*8)); }
static void write_packet(struct result *r, uint8_t codec, uint8_t flags, uint32_t id,
    uint16_t width, uint16_t height, uint64_t pts, uint16_t epoch, const uint8_t *data, size_t size)
{
    assert(size && size <= UINT32_MAX);
    uint8_t header[NP_MEDIA_HEADER_SIZE]={'N','P','E','N',NP_MEDIA_VERSION,codec,flags,0};
    put32(header+8,1); put32(header+12,id); put32(header+16,(uint32_t)width|((uint32_t)height<<16));
    put32(header+20,pts); put32(header+24,pts>>32); put32(header+28,size); put32(header+32,epoch);
    assert(fwrite(header, 1, sizeof(header), r->file) == sizeof(header));
    assert(fwrite(data, 1, size, r->file) == size);
}
static void output(void *user, const uint8_t *data, size_t size, uint64_t pts, uint32_t id,
    uint8_t flags, const uint8_t *alpha, uint32_t alpha_size, uint16_t epoch,
    uint16_t width, uint16_t height, enum np_encoder_codec codec)
{
    struct result *r = user;
    r->elapsed = (now() - r->start) * 1000;
    assert(codec == (id == 93 ? NP_ENCODER_AV1 : NP_ENCODER_H264));
    assert((flags & NP_ENCODER_FLAG_HARDWARE) != 0 || id == 93);
    if (id >= 91 && id <= 94) {
        assert(epoch != r->epoch && !(flags & NP_ENCODER_FLAG_REUSE_ALPHA));
    } else if (id > 1) assert(epoch == r->epoch);
    r->epoch = epoch;
    if (alpha_size) write_packet(r, NP_MEDIA_CODEC_ALPHA_RLE, 0, id, width, height, pts, epoch, alpha, alpha_size);
    write_packet(r, codec, flags, id, width, height, pts, epoch, data, size);
}
static void done(void *user, bool ok)
{
    struct result *r = user; assert(ok);
    pthread_mutex_lock(&r->lock); r->done = true;
    pthread_cond_signal(&r->ready); pthread_mutex_unlock(&r->lock);
}
static int compare(const void *a, const void *b) { double x=*(const double *)a, y=*(const double *)b; return (x>y)-(x<y); }
int main(int argc, char **argv)
{
    assert(argc == 2);
    struct result r = { .lock=PTHREAD_MUTEX_INITIALIZER, .ready=PTHREAD_COND_INITIALIZER,
        .file=fopen(argv[1], "wb") };
    assert(r.file);
    struct np_encoder *encoder = np_encoder_create(1656, 1258, true, output, done, &r); assert(encoder);
    double samples[80], total = 0, first = 0;
    for (unsigned frame = 0; frame < 95; frame++) {
        int w = frame == 90 ? 701 : frame == 91 ? 913 : frame == 92 ? 16 : 1656;
        int h = frame == 90 ? 503 : frame == 91 ? 617 : frame == 92 ? 16 : 1258;
        int even_w=(w+1)&~1, even_h=(h+1)&~1;
        uint8_t *pixels=malloc((size_t)even_w*even_h*4); assert(pixels);
        for (int y=0; y<even_h; y++) for (int x=0; x<even_w; x++) {
            uint8_t *p=pixels+((size_t)y*even_w+x)*4;
            int bar=(y/43)%12, moving=(x+(int)frame*7+bar*71)%240<80;
            p[0]=moving?220-bar*9:30; p[1]=moving?180:18; p[2]=moving?40+bar*10:10;
            if (y%80>60 && x%21<2) p[0]=p[1]=p[2]=220;
            p[3]=x<8&&y<8 ? (frame==94?128:0) : 255;
        }
        r.start=now();
        assert(np_encoder_take_bgra(encoder, pixels, even_w, even_h, even_w*4,
            frame*16666667ull, frame+1, NP_ENCODER_FLAG_HAS_ALPHA));
        pthread_mutex_lock(&r.lock);
        struct timespec limit; clock_gettime(CLOCK_REALTIME,&limit); limit.tv_sec+=15;
        while (!r.done) assert(pthread_cond_timedwait(&r.ready,&r.lock,&limit)==0);
        r.done=false;
        if (!frame) first=r.elapsed;
        if (frame>=10&&frame<90) { samples[frame-10]=r.elapsed; total+=r.elapsed; }
        pthread_mutex_unlock(&r.lock);
    }
    np_encoder_destroy(encoder); assert(fclose(r.file)==0);
    qsort(samples,80,sizeof(double),compare);
    printf("NVENC_PRODUCTION frames=95 hardware_frames=94 samples=80 first_ms=%.3f mean_ms=%.3f p50_ms=%.3f p95_ms=%.3f max_ms=%.3f\n",
        first,total/80,samples[40],samples[75],samples[79]);
    pthread_mutex_destroy(&r.lock); pthread_cond_destroy(&r.ready);
}
