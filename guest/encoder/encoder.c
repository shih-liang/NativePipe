/* Negotiated remote encoding. The worker owns each accepted pixel buffer through
 * conversion/encode; codec backends never own scene or transport scheduling. */
#include "encoder.h"
#include "alpha.h"
#include "encoder_vaapi.h"
#include "encoder_nvenc.h"
#include <aom/aom_encoder.h>
#include <aom/aomcx.h>
#include <libyuv/convert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct np_encoder {
    int width, height;
    uint16_t epoch;
    np_encoder_output_fn out;
    np_encoder_done_fn done;
    void *user;
    struct np_av1_vaapi *hardware;
    struct np_h264_nvenc *nvenc;
    bool allow_h264, nvenc_failed;
    bool hardware_failed, codec_ready;
    aom_codec_ctx_t codec;
    aom_image_t *image;
    bool force_keyframe;
    uint8_t *last_alpha;
    uint32_t last_alpha_size;
    uint16_t last_alpha_epoch;
    int64_t frame_number;
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    bool worker_started, stopping;
    uint8_t *pending_bgra;
    int pending_width, pending_height, pending_stride;
    uint64_t pending_pts;
    uint32_t pending_resource_id;
    uint8_t pending_flags;
    uint16_t pending_epoch;
    bool pending_force, encoding;
    int requested_width, requested_height;
    uint16_t advertised_epoch;
};

static void teardown_codec(struct np_encoder *enc) {
    np_h264_nvenc_destroy(enc->nvenc); enc->nvenc = NULL;
    np_av1_vaapi_destroy(enc->hardware); enc->hardware = NULL;
    if (enc->codec_ready) aom_codec_destroy(&enc->codec);
    enc->codec_ready = false;
    if (enc->image) aom_img_free(enc->image);
    enc->image = NULL;
}

static bool setup_software(struct np_encoder *enc) {
    aom_codec_enc_cfg_t cfg;
    if (aom_codec_enc_config_default(aom_codec_av1_cx(), &cfg, AOM_USAGE_REALTIME)) return false;
    cfg.g_w = enc->width; cfg.g_h = enc->height;
    cfg.g_timebase.num = 1; cfg.g_timebase.den = 60;
    cfg.g_threads = 2;
    cfg.g_lag_in_frames = 0;
    cfg.rc_end_usage = AOM_CBR;
    cfg.rc_target_bitrate = 4000;
    cfg.rc_min_quantizer = 8; cfg.rc_max_quantizer = 52;
    cfg.rc_dropframe_thresh = 0;
    cfg.rc_buf_sz = 250; cfg.rc_buf_initial_sz = 125; cfg.rc_buf_optimal_sz = 125;
    cfg.kf_min_dist = 0; cfg.kf_max_dist = 120;
    if (aom_codec_enc_init(&enc->codec, aom_codec_av1_cx(), &cfg, 0)) return false;
    enc->codec_ready = true;
    if (aom_codec_control(&enc->codec, AOME_SET_CPUUSED, 10) ||
        aom_codec_control(&enc->codec, AV1E_SET_ROW_MT, 1u) ||
        aom_codec_control(&enc->codec, AV1E_SET_TUNE_CONTENT, AOM_CONTENT_SCREEN) ||
        aom_codec_control(&enc->codec, AV1E_SET_ENABLE_TPL_MODEL, 0u) ||
        aom_codec_control(&enc->codec, AV1E_SET_ENABLE_RESTORATION, 0u) ||
        aom_codec_control(&enc->codec, AV1E_SET_COLOR_RANGE, AOM_CR_STUDIO_RANGE) ||
        aom_codec_control(&enc->codec, AV1E_SET_MATRIX_COEFFICIENTS, AOM_CICP_MC_BT_601)) return false;
    fprintf(stderr, "[encoder] AV1 libaom realtime %dx%d\n", enc->width, enc->height);
    return true;
}

static bool setup_av1(struct np_encoder *enc) {
    enc->image = aom_img_alloc(NULL, AOM_IMG_FMT_I420, enc->width, enc->height, 32);
    if (!enc->image) return false;
    if (!enc->hardware_failed && !getenv("NATIVEPIPE_SOFTWARE_ENCODER") &&
        np_av1_vaapi_supports_size(enc->width, enc->height)) {
        enc->hardware = np_av1_vaapi_create(enc->width, enc->height);
        enc->hardware_failed = !enc->hardware;
    }
    return enc->hardware || setup_software(enc);
}

static void restart_epoch(struct np_encoder *enc) {
    enc->epoch++; if (!enc->epoch) enc->epoch = 1;
    pthread_mutex_lock(&enc->lock);
    enc->advertised_epoch = enc->epoch;
    pthread_mutex_unlock(&enc->lock);
}

static bool encode_bgra_now(struct np_encoder *enc, const uint8_t *bgra, int width, int height,
    int stride, uint64_t pts, uint32_t resource, uint8_t flags,
    const uint8_t *alpha, uint32_t alpha_size, uint16_t epoch, bool force) {
    if ((!enc->image && !enc->nvenc) || width != enc->width || height != enc->height) {
        teardown_codec(enc);
        enc->width = width; enc->height = height; enc->frame_number = 0;
        if (enc->allow_h264 && !enc->nvenc_failed && !getenv("NATIVEPIPE_SOFTWARE_ENCODER") &&
            np_h264_nvenc_supports_size(width, height)) {
            enc->nvenc = np_h264_nvenc_create(width, height);
            enc->nvenc_failed = !enc->nvenc;
            if (!enc->nvenc) fprintf(stderr, "[encoder] NVENC unavailable; using AV1\n");
        }
        if (!enc->nvenc && !setup_av1(enc)) return false;
        force = true;
    }
    enc->epoch = epoch;
    uint8_t *packet = NULL; size_t size = 0;
    if (enc->nvenc && !np_h264_nvenc_encode(enc->nvenc, bgra, stride,
        force || enc->force_keyframe, &packet, &size)) {
        np_h264_nvenc_destroy(enc->nvenc); enc->nvenc = NULL;
        enc->nvenc_failed = true;
        if (!setup_av1(enc)) return false;
        restart_epoch(enc);
        flags &= ~NP_ENCODER_FLAG_REUSE_ALPHA;
        force = true;
        fprintf(stderr, "[encoder] NVENC failed; reset to AV1 epoch=%u\n", enc->epoch);
    }
    if (enc->nvenc) goto emit;
    if (ARGBToI420(bgra, stride, enc->image->planes[0], enc->image->stride[0],
        enc->image->planes[1], enc->image->stride[1], enc->image->planes[2], enc->image->stride[2],
        width, height)) return false;
    enc->image->range = AOM_CR_STUDIO_RANGE;
    enc->image->mc = AOM_CICP_MC_BT_601;
    if (enc->hardware && !np_av1_vaapi_encode(enc->hardware,
        (const uint8_t **)enc->image->planes, enc->image->stride,
        force || enc->force_keyframe, &packet, &size)) {
        // A new stream starts with a keyframe; no old hardware references escape.
        np_av1_vaapi_destroy(enc->hardware); enc->hardware = NULL;
        enc->hardware_failed = true;
        if (!setup_software(enc)) return false;
        restart_epoch(enc);
        flags &= ~NP_ENCODER_FLAG_REUSE_ALPHA;
        force = true;
        fprintf(stderr, "[encoder] AV1 hardware failed; reset to software epoch=%u\n", enc->epoch);
    }
    if (!enc->hardware) {
        if (aom_codec_encode(&enc->codec, enc->image, enc->frame_number++, 1,
            (force || enc->force_keyframe) ? AOM_EFLAG_FORCE_KF : 0)) return false;
        aom_codec_iter_t iter = NULL;
        const aom_codec_cx_pkt_t *p;
        while ((p = aom_codec_get_cx_data(&enc->codec, &iter))) {
            if (p->kind != AOM_CODEC_CX_FRAME_PKT) continue;
            // The protocol requires one immediately displayed frame per resource.
            if (size || !p->data.frame.sz || p->data.frame.sz > 32u * 1024u * 1024u) {
                free(packet); return false;
            }
            size = p->data.frame.sz;
            packet = malloc(size);
            if (!packet) return false;
            memcpy(packet, p->data.frame.buf, size);
        }
    }
emit:
    if (!size) { free(packet); return false; }
    if (enc->nvenc || enc->hardware) flags |= NP_ENCODER_FLAG_HARDWARE;
    bool reuse = flags & NP_ENCODER_FLAG_REUSE_ALPHA;
    enc->out(enc->user, packet, size, pts, resource, flags,
        reuse ? NULL : alpha, reuse ? 0 : alpha_size, enc->epoch, width, height,
        enc->nvenc ? NP_ENCODER_H264 : NP_ENCODER_AV1);
    free(packet);
    enc->force_keyframe = false;
    return true;
}

static void *encoder_worker(void *arg) {
	struct np_encoder *enc = arg;
	for (;;) {
		pthread_mutex_lock(&enc->lock);
		while (!enc->stopping && !enc->pending_bgra)
			pthread_cond_wait(&enc->ready, &enc->lock);
		if (enc->stopping && !enc->pending_bgra) {
			pthread_mutex_unlock(&enc->lock);
			break;
		}
		uint8_t *pixels = enc->pending_bgra;
		int width = enc->pending_width, height = enc->pending_height;
		int stride = enc->pending_stride;
		uint64_t pts = enc->pending_pts;
		uint32_t resource_id = enc->pending_resource_id;
		uint8_t flags = enc->pending_flags;
		uint16_t epoch = enc->pending_epoch;
		bool force = enc->pending_force;
		enc->pending_force = false;
		enc->pending_bgra = NULL;
		enc->encoding = true;
		pthread_mutex_unlock(&enc->lock);

		uint8_t *alpha = NULL;
		uint32_t alpha_size = 0;
		bool alpha_ok = !(flags & NP_ENCODER_FLAG_HAS_ALPHA) || np_alpha_pack_bgra(
			pixels, width, height, stride, &alpha, &alpha_size);
		bool reuse_alpha = alpha && epoch == enc->last_alpha_epoch &&
			alpha_size == enc->last_alpha_size && enc->last_alpha &&
			!memcmp(alpha, enc->last_alpha, alpha_size);
		if (reuse_alpha) flags |= NP_ENCODER_FLAG_REUSE_ALPHA;
		bool encoded = alpha_ok && encode_bgra_now(
			enc, pixels, width, height, stride, pts, resource_id, flags,
			alpha, alpha_size, epoch, force);
		if (!encoded)
			fprintf(stderr, "[encoder] frame encode failed\n");
		if (encoded && alpha && (!reuse_alpha || epoch != enc->epoch)) {
			free(enc->last_alpha); enc->last_alpha = alpha; alpha = NULL;
			enc->last_alpha_size = alpha_size; enc->last_alpha_epoch = enc->epoch;
		}
		free(alpha);
		pthread_mutex_lock(&enc->lock);
		enc->encoding = false;
		pthread_mutex_unlock(&enc->lock);
		free(pixels);
		enc->done(enc->user, encoded);
	}
	return NULL;
}

struct np_encoder *np_encoder_create(int width, int height, bool allow_h264, np_encoder_output_fn out,
                                    np_encoder_done_fn done, void *user) {
	if (width < 2 || height < 2 || width > 8192 || height > 8192 || (uint64_t)width * height > 16777216 || !out || !done) return NULL;
	struct np_encoder *enc = calloc(1, sizeof(*enc));
	if (!enc) return NULL;
	enc->width = width & ~1;
	enc->height = height & ~1;
	enc->out = out;
	enc->done = done;
	enc->user = user;
	enc->allow_h264 = allow_h264;
	enc->epoch = 1;
	enc->advertised_epoch = 1;
	enc->requested_width = enc->width;
	enc->requested_height = enc->height;
	enc->force_keyframe = true;
	pthread_mutex_init(&enc->lock, NULL);
	pthread_cond_init(&enc->ready, NULL);
	if (pthread_create(&enc->worker, NULL, encoder_worker, enc) != 0) {
		np_encoder_destroy(enc);
		return NULL;
	}
	enc->worker_started = true;
	return enc;
}

void np_encoder_destroy(struct np_encoder *enc) {
	if (!enc) return;
	pthread_mutex_lock(&enc->lock);
	enc->stopping = true;
	pthread_cond_signal(&enc->ready);
	pthread_mutex_unlock(&enc->lock);
	if (enc->worker_started) pthread_join(enc->worker, NULL);
	free(enc->pending_bgra);
	free(enc->last_alpha);
	teardown_codec(enc);
	pthread_cond_destroy(&enc->ready);
	pthread_mutex_destroy(&enc->lock);
	free(enc);
}

uint16_t np_encoder_epoch(const struct np_encoder *enc) {
    if (!enc) return 0;
    struct np_encoder *e = (struct np_encoder *)enc;
    pthread_mutex_lock(&e->lock);
    uint16_t epoch = e->advertised_epoch;
    pthread_mutex_unlock(&e->lock);
    return epoch;
}

bool np_encoder_push_bgra(struct np_encoder *enc, const uint8_t *bgra, int width, int height,
	                          int stride, uint64_t pts_ns, uint32_t resource_id,
	                          uint8_t flags) {
	if (!enc || !bgra || !resource_id || width < 2 || height < 2 ||
	    width > 8192 || height > 8192 || (uint64_t)width * height > 16777216 || stride < width * 4)
		return false;
	int even_width = (width + 1) & ~1;
	int even_height = (height + 1) & ~1;
	size_t packed_stride = (size_t)even_width * 4;
	if ((size_t)even_height > SIZE_MAX / packed_stride) return false;
	uint8_t *copy = malloc(packed_stride * (size_t)even_height);
	if (!copy) return false;
	for (int row = 0; row < height; row++) {
		uint8_t *dst = copy + (size_t)row * packed_stride;
		memcpy(dst, bgra + (size_t)row * (size_t)stride, (size_t)width * 4);
		if (even_width != width) memcpy(dst + (size_t)width * 4, dst + (size_t)(width - 1) * 4, 4);
	}
	if (even_height != height)
		memcpy(copy + (size_t)height * packed_stride,
		       copy + (size_t)(height - 1) * packed_stride, packed_stride);

	bool accepted = np_encoder_take_bgra(enc, copy, even_width, even_height,
	    (int)packed_stride, pts_ns, resource_id, flags);
	if (!accepted) free(copy);
	return accepted;
}

bool np_encoder_take_bgra(struct np_encoder *enc, uint8_t *pixels, int width, int height,
                         int stride, uint64_t pts_ns, uint32_t resource_id, uint8_t flags) {
	if (!enc || !pixels || !resource_id || width < 2 || height < 2 ||
	    width > 8192 || height > 8192 || (uint64_t)width * height > 16777216 || (width & 1) || (height & 1) ||
	    stride < width * 4) return false;
	pthread_mutex_lock(&enc->lock);
	if (enc->pending_bgra || enc->encoding || enc->stopping) {
		pthread_mutex_unlock(&enc->lock);
		return false;
	}
	if (width != enc->requested_width || height != enc->requested_height) {
		enc->requested_width = width;
		enc->requested_height = height;
		enc->advertised_epoch++;
        if (!enc->advertised_epoch) enc->advertised_epoch = 1;
		enc->pending_force = true;
	}
	enc->pending_bgra = pixels;
	enc->pending_width = width;
	enc->pending_height = height;
	enc->pending_stride = stride;
	enc->pending_pts = pts_ns;
	enc->pending_resource_id = resource_id;
	enc->pending_flags = flags;
	enc->pending_epoch = enc->advertised_epoch;
	pthread_cond_signal(&enc->ready);
	pthread_mutex_unlock(&enc->lock);
	return true;
}
