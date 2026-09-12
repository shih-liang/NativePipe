/* Fault injection only: verify a mid-stream hardware failure rebuilds AV1 and
 * alpha in a fresh epoch. Real NVENC is exercised by hardware_encoder_test. */
#include "../../encoder/encoder.h"
#include "../../encoder/encoder_nvenc.h"
#include "../../encoder/encoder_vaapi.h"
#include <dav1d/dav1d.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct np_h264_nvenc { int unused; };
static int opens, submissions, closes;
bool np_h264_nvenc_supports_size(int w,int h) { return w>=2&&h>=2; }
struct np_h264_nvenc *np_h264_nvenc_create(int w,int h) { (void)w;(void)h;opens++;return calloc(1,sizeof(struct np_h264_nvenc)); }
void np_h264_nvenc_destroy(struct np_h264_nvenc *e) { if(e) {closes++;free(e);} }
bool np_h264_nvenc_encode(struct np_h264_nvenc *e,const uint8_t *p,int stride,bool key,uint8_t **out,size_t *size)
{
    (void)e;(void)p;(void)stride;
    if(++submissions!=1) return false;
    assert(key); *size=1; *out=malloc(1); assert(*out); **out=0; return true;
}
bool np_av1_vaapi_supports_size(int w,int h) { (void)w;(void)h;return false; }
struct np_av1_vaapi *np_av1_vaapi_create(int w,int h) { (void)w;(void)h;return NULL; }
void np_av1_vaapi_destroy(struct np_av1_vaapi *e) { assert(!e); }
bool np_av1_vaapi_encode(struct np_av1_vaapi *e,const uint8_t *p[3],const int s[3],bool k,uint8_t **o,size_t *n)
{ (void)e;(void)p;(void)s;(void)k;(void)o;(void)n;abort(); }

struct result { pthread_mutex_t lock; pthread_cond_t ready; bool done; unsigned frames; uint16_t epoch; Dav1dContext *decoder; };
static void output(void *user,const uint8_t *p,size_t size,uint64_t pts,uint32_t id,
    uint8_t flags,const uint8_t *alpha,uint32_t alpha_size,uint16_t epoch,uint16_t w,uint16_t h,enum np_encoder_codec codec)
{
    (void)pts;
    struct result *r=user;
    assert(w==(id==4?64:128)&&h==96);
    assert(codec==(id==1?NP_ENCODER_H264:NP_ENCODER_AV1));
    assert(!!(flags&NP_ENCODER_FLAG_HARDWARE)==(id==1));
    if(id==3) { assert(epoch==r->epoch && (flags&NP_ENCODER_FLAG_REUSE_ALPHA)); assert(!alpha&&!alpha_size); }
    else { assert(epoch!=r->epoch && !(flags&NP_ENCODER_FLAG_REUSE_ALPHA)); assert(alpha&&alpha_size); }
    if(id>1) {
        if(epoch!=r->epoch) {
            if(r->decoder) dav1d_close(&r->decoder);
            Dav1dSettings settings; dav1d_default_settings(&settings);
            settings.n_threads=settings.max_frame_delay=1;
            assert(dav1d_open(&r->decoder,&settings)==0);
        }
        Dav1dData data={0}; uint8_t *copy=dav1d_data_create(&data,size); assert(copy); memcpy(copy,p,size);
        assert(dav1d_send_data(r->decoder,&data)==0); dav1d_data_unref(&data);
        Dav1dPicture picture={0}; assert(dav1d_get_picture(r->decoder,&picture)==0);
        assert(picture.p.w==w&&picture.p.h==h); dav1d_picture_unref(&picture);
    }
    r->epoch=epoch; r->frames++;
}
static void done(void *user,bool ok) {
    struct result *r=user;assert(ok);pthread_mutex_lock(&r->lock);r->done=true;
    pthread_cond_signal(&r->ready);pthread_mutex_unlock(&r->lock);
}
int main(void) {
    unsetenv("NATIVEPIPE_SOFTWARE_ENCODER");
    struct result r={.lock=PTHREAD_MUTEX_INITIALIZER,.ready=PTHREAD_COND_INITIALIZER};
    struct np_encoder *e=np_encoder_create(128,96,true,output,done,&r);assert(e);
    for(unsigned id=1;id<=4;id++) {
        int w=id==4?64:128;
        uint8_t *pixels=malloc(w*96*4);assert(pixels);memset(pixels,120,w*96*4);
        assert(np_encoder_take_bgra(e,pixels,w,96,w*4,id,id,NP_ENCODER_FLAG_HAS_ALPHA));
        pthread_mutex_lock(&r.lock);struct timespec limit;clock_gettime(CLOCK_REALTIME,&limit);limit.tv_sec+=10;
        while(!r.done) assert(pthread_cond_timedwait(&r.ready,&r.lock,&limit)==0);
        r.done=false;pthread_mutex_unlock(&r.lock);
    }
    np_encoder_destroy(e);dav1d_close(&r.decoder);
    assert(r.frames==4&&opens==1&&submissions==2&&closes==1);
    pthread_mutex_destroy(&r.lock);pthread_cond_destroy(&r.ready);
    puts("Injected NVENC failure: AV1 keyframe/epoch/alpha reset and no repeated hardware probing: PASS");
}
