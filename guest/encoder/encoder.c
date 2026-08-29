/*
 * H.264 encoder for NativePipe remote display.
 *
 * Prefers h264_vaapi when a VA-API device is available; falls back to libx264
 * (software) so GTK/cairo shm clients still encode on machines without
 * working hardware encode.
 */
#include "encoder.h"
#include "alpha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

struct np_encoder {
	int width;
	int height;
	uint16_t epoch;
	np_encoder_output_fn out;
	void *user;

	bool use_vaapi;
	const AVCodec *codec;
	AVCodecContext *ctx;
	AVBufferRef *hw_device_ctx;
	AVBufferRef *hw_frames_ctx;
	AVFrame *sw_frame;
	AVFrame *hw_frame;
	AVPacket *packet;
	struct SwsContext *sws;
	bool force_keyframe;

	/* Wayland submits only the newest frame; encoding runs off its event loop. */
	pthread_t worker;
	pthread_mutex_t lock;
	pthread_cond_t ready;
	bool worker_started;
	bool stopping;
	uint8_t *pending_bgra;
	int pending_width, pending_height, pending_stride;
	uint64_t pending_pts;
	uint32_t pending_resource_id;
	uint8_t pending_flags;
	uint16_t pending_epoch;
	bool pending_force;
	uint8_t *encoding_bgra;
	int encoding_width, encoding_height, encoding_stride;
	uint32_t encoding_resource_id;
	uint8_t encoding_flags;
	uint8_t *last_bgra;
	int last_width, last_height, last_stride;
	uint8_t last_flags;
	int requested_width, requested_height;
	uint16_t advertised_epoch;
};

static bool setup_libx264(struct np_encoder *enc) {
	enc->codec = avcodec_find_encoder_by_name("libx264");
	if (!enc->codec) enc->codec = avcodec_find_encoder(AV_CODEC_ID_H264);
	if (!enc->codec) {
		fprintf(stderr, "[encoder] no H.264 encoder\n");
		return false;
	}
	enc->ctx = avcodec_alloc_context3(enc->codec);
	if (!enc->ctx) return false;
	enc->ctx->width = enc->width;
	enc->ctx->height = enc->height;
	enc->ctx->time_base = (AVRational){1, 60};
	enc->ctx->framerate = (AVRational){60, 1};
	enc->ctx->bit_rate = 8 * 1000 * 1000;
	enc->ctx->gop_size = 60;
	enc->ctx->max_b_frames = 0;
	enc->ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	if (enc->codec->id == AV_CODEC_ID_H264) {
		av_opt_set(enc->ctx->priv_data, "preset", "veryfast", 0);
		av_opt_set(enc->ctx->priv_data, "tune", "zerolatency", 0);
		/* Prefer Annex-B start codes so the macOS demuxer can split NALs. */
		av_opt_set(enc->ctx->priv_data, "annexb", "1", 0);
		av_opt_set(enc->ctx->priv_data, "repeat-headers", "1", 0);
		av_opt_set(enc->ctx->priv_data, "slices", "1", 0);
	}
	if (avcodec_open2(enc->ctx, enc->codec, NULL) < 0) {
		avcodec_free_context(&enc->ctx);
		return false;
	}
	enc->sw_frame = av_frame_alloc();
	enc->packet = av_packet_alloc();
	if (!enc->sw_frame || !enc->packet) return false;
	enc->sw_frame->format = AV_PIX_FMT_YUV420P;
	enc->sw_frame->width = enc->width;
	enc->sw_frame->height = enc->height;
	if (av_frame_get_buffer(enc->sw_frame, 32) < 0) return false;
	enc->sws = sws_getContext(enc->width, enc->height, AV_PIX_FMT_BGRA,
	                          enc->width, enc->height, AV_PIX_FMT_YUV420P,
	                          SWS_BILINEAR, NULL, NULL, NULL);
	enc->use_vaapi = false;
	return enc->sws != NULL;
}

static bool setup_vaapi(struct np_encoder *enc) {
	enc->codec = avcodec_find_encoder_by_name("h264_vaapi");
	if (!enc->codec) return false;

	if (av_hwdevice_ctx_create(&enc->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
	                           NULL, NULL, 0) < 0) {
		return false;
	}

	enc->ctx = avcodec_alloc_context3(enc->codec);
	if (!enc->ctx) return false;
	enc->ctx->width = enc->width;
	enc->ctx->height = enc->height;
	enc->ctx->time_base = (AVRational){1, 60};
	enc->ctx->framerate = (AVRational){60, 1};
	enc->ctx->bit_rate = 8 * 1000 * 1000;
	enc->ctx->gop_size = 60;
	enc->ctx->max_b_frames = 0;
	enc->ctx->pix_fmt = AV_PIX_FMT_VAAPI;
	enc->ctx->hw_device_ctx = av_buffer_ref(enc->hw_device_ctx);

	enc->hw_frames_ctx = av_hwframe_ctx_alloc(enc->hw_device_ctx);
	if (!enc->hw_frames_ctx) return false;
	AVHWFramesContext *frames = (AVHWFramesContext *)enc->hw_frames_ctx->data;
	frames->format = AV_PIX_FMT_VAAPI;
	frames->sw_format = AV_PIX_FMT_NV12;
	frames->width = enc->width;
	frames->height = enc->height;
	frames->initial_pool_size = 4;
	if (av_hwframe_ctx_init(enc->hw_frames_ctx) < 0) return false;
	enc->ctx->hw_frames_ctx = av_buffer_ref(enc->hw_frames_ctx);

	if (avcodec_open2(enc->ctx, enc->codec, NULL) < 0) return false;

	enc->sw_frame = av_frame_alloc();
	enc->hw_frame = av_frame_alloc();
	enc->packet = av_packet_alloc();
	if (!enc->sw_frame || !enc->hw_frame || !enc->packet) return false;
	enc->sw_frame->format = AV_PIX_FMT_NV12;
	enc->sw_frame->width = enc->width;
	enc->sw_frame->height = enc->height;
	if (av_frame_get_buffer(enc->sw_frame, 32) < 0) return false;
	if (av_hwframe_get_buffer(enc->hw_frames_ctx, enc->hw_frame, 0) < 0) return false;

	enc->sws = sws_getContext(enc->width, enc->height, AV_PIX_FMT_BGRA,
	                          enc->width, enc->height, AV_PIX_FMT_NV12,
	                          SWS_BILINEAR, NULL, NULL, NULL);
	enc->use_vaapi = true;
	return enc->sws != NULL;
}

static void teardown_codec(struct np_encoder *enc) {
	if (enc->sws) {
		sws_freeContext(enc->sws);
		enc->sws = NULL;
	}
	av_packet_free(&enc->packet);
	av_frame_free(&enc->hw_frame);
	av_frame_free(&enc->sw_frame);
	avcodec_free_context(&enc->ctx);
	av_buffer_unref(&enc->hw_frames_ctx);
	av_buffer_unref(&enc->hw_device_ctx);
	enc->use_vaapi = false;
}

static bool setup(struct np_encoder *enc) {
	if (setup_vaapi(enc)) {
		fprintf(stderr, "[encoder] h264_vaapi %dx%d\n", enc->width, enc->height);
		return true;
	}
	teardown_codec(enc);
	if (setup_libx264(enc)) {
		fprintf(stderr, "[encoder] libx264 %dx%d\n", enc->width, enc->height);
		return true;
	}
	return false;
}

static bool encode_bgra_now(struct np_encoder *enc, const uint8_t *bgra, int width, int height,
	                            int stride, uint64_t pts_ns, uint32_t resource_id,
	                            uint8_t flags, const uint8_t *alpha,
	                            uint32_t alpha_size, uint16_t epoch, bool force);

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
		enc->encoding_bgra = pixels;
		enc->encoding_width = width;
		enc->encoding_height = height;
		enc->encoding_stride = stride;
		enc->encoding_resource_id = resource_id;
		enc->encoding_flags = flags;
		pthread_mutex_unlock(&enc->lock);

		uint8_t *alpha = NULL;
		uint32_t alpha_size = 0;
		bool alpha_ok = !(flags & NP_ENCODER_FLAG_HAS_ALPHA) || np_alpha_pack_bgra(
			pixels, width, height, stride, &alpha, &alpha_size);
		bool encoded = alpha_ok && encode_bgra_now(
			enc, pixels, width, height, stride, pts, resource_id, flags,
			alpha, alpha_size, epoch, force);
		if (!encoded)
			fprintf(stderr, "[encoder] frame encode failed\n");
		free(alpha);
		pthread_mutex_lock(&enc->lock);
		enc->encoding_bgra = NULL;
		if (encoded) {
			free(enc->last_bgra);
			enc->last_bgra = pixels;
			enc->last_width = width;
			enc->last_height = height;
			enc->last_stride = stride;
			enc->last_flags = flags;
			pixels = NULL;
		}
		pthread_mutex_unlock(&enc->lock);
		free(pixels);
	}
	return NULL;
}

struct np_encoder *np_encoder_create(int width, int height, np_encoder_output_fn out, void *user) {
	if (width < 2 || height < 2 || !out) return NULL;
	struct np_encoder *enc = calloc(1, sizeof(*enc));
	if (!enc) return NULL;
	enc->width = width & ~1;
	enc->height = height & ~1;
	enc->out = out;
	enc->user = user;
	enc->epoch = 1;
	enc->advertised_epoch = 1;
	enc->requested_width = enc->width;
	enc->requested_height = enc->height;
	enc->force_keyframe = true;
	pthread_mutex_init(&enc->lock, NULL);
	pthread_cond_init(&enc->ready, NULL);
	if (!setup(enc)) {
		np_encoder_destroy(enc);
		return NULL;
	}
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
	free(enc->last_bgra);
	teardown_codec(enc);
	pthread_cond_destroy(&enc->ready);
	pthread_mutex_destroy(&enc->lock);
	free(enc);
}

bool np_encoder_resize(struct np_encoder *enc, int width, int height) {
	if (!enc) return false;
	width &= ~1;
	height &= ~1;
	if (width < 2 || height < 2) return false;
	if (width == enc->width && height == enc->height) return true;
	teardown_codec(enc);
	enc->width = width;
	enc->height = height;
	enc->epoch++;
	if (!setup(enc)) return false;
	fprintf(stderr, "[encoder] resized %dx%d epoch=%u\n", enc->width, enc->height, enc->epoch);
	return true;
}

uint16_t np_encoder_epoch(const struct np_encoder *enc) {
	if (!enc) return 0;
	struct np_encoder *mutable = (struct np_encoder *)enc;
	pthread_mutex_lock(&mutable->lock);
	uint16_t epoch = mutable->advertised_epoch;
	pthread_mutex_unlock(&mutable->lock);
	return epoch;
}

void np_encoder_force_keyframe(struct np_encoder *enc) {
	if (!enc) return;
	pthread_mutex_lock(&enc->lock);
	enc->pending_force = true;
	pthread_mutex_unlock(&enc->lock);
}

bool np_encoder_replay_last(
	struct np_encoder *enc, uint64_t pts_ns, uint32_t replacement_resource_id,
	uint32_t *active_resource_id)
{
	if (active_resource_id) *active_resource_id = 0;
	if (!enc || !replacement_resource_id || !active_resource_id) return false;
	pthread_mutex_lock(&enc->lock);
	enc->pending_force = true;
	if (enc->pending_bgra) {
		*active_resource_id = enc->pending_resource_id;
		pthread_cond_signal(&enc->ready);
		pthread_mutex_unlock(&enc->lock);
		return true;
	}
	const uint8_t *source = enc->encoding_bgra
		? enc->encoding_bgra : enc->last_bgra;
	int width = enc->encoding_bgra ? enc->encoding_width : enc->last_width;
	int height = enc->encoding_bgra ? enc->encoding_height : enc->last_height;
	int stride = enc->encoding_bgra ? enc->encoding_stride : enc->last_stride;
	uint8_t flags = enc->encoding_bgra ? enc->encoding_flags : enc->last_flags;
	if (!source || width <= 0 || height <= 0 || stride <= 0 ||
	    (size_t)height > SIZE_MAX / (size_t)stride) {
		pthread_mutex_unlock(&enc->lock);
		return false;
	}
	size_t size = (size_t)height * (size_t)stride;
	uint8_t *copy = malloc(size);
	if (!copy) {
		pthread_mutex_unlock(&enc->lock);
		return false;
	}
	memcpy(copy, source, size);
	enc->pending_bgra = copy;
	enc->pending_width = width;
	enc->pending_height = height;
	enc->pending_stride = stride;
	enc->pending_pts = pts_ns;
	enc->pending_resource_id = replacement_resource_id;
	enc->pending_flags = flags;
	enc->pending_epoch = enc->advertised_epoch;
	*active_resource_id = replacement_resource_id;
	pthread_cond_signal(&enc->ready);
	pthread_mutex_unlock(&enc->lock);
	return true;
}

static bool drain_packets(struct np_encoder *enc, uint64_t pts_ns,
	                          uint32_t resource_id, uint8_t flags,
	                          const uint8_t *alpha, uint32_t alpha_size) {
	bool first = true;
	for (;;) {
		int ret = avcodec_receive_packet(enc->ctx, enc->packet);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
		if (ret < 0) return false;
		enc->out(enc->user, enc->packet->data, (size_t)enc->packet->size,
		         pts_ns, resource_id, flags,
		         first ? alpha : NULL, first ? alpha_size : 0, enc->epoch,
		         (uint16_t)enc->width, (uint16_t)enc->height);
		first = false;
		av_packet_unref(enc->packet);
	}
}

static bool encode_bgra_now(struct np_encoder *enc, const uint8_t *bgra, int width, int height,
	                            int stride, uint64_t pts_ns, uint32_t resource_id,
	                            uint8_t flags, const uint8_t *alpha,
	                            uint32_t alpha_size, uint16_t epoch, bool force) {
	if (!enc || !bgra) return false;
	if ((width & ~1) != enc->width || (height & ~1) != enc->height) {
		if (!np_encoder_resize(enc, width, height)) return false;
	}
	enc->epoch = epoch;
	if (av_frame_make_writable(enc->sw_frame) < 0) return false;
	const uint8_t *src_slices[4] = {bgra, NULL, NULL, NULL};
	int src_stride[4] = {stride, 0, 0, 0};
	sws_scale(enc->sws, src_slices, src_stride, 0, enc->height,
	          enc->sw_frame->data, enc->sw_frame->linesize);
	enc->sw_frame->pts++;
	if (force || enc->force_keyframe) {
		enc->sw_frame->pict_type = AV_PICTURE_TYPE_I;
		enc->force_keyframe = false;
	} else {
		enc->sw_frame->pict_type = AV_PICTURE_TYPE_NONE;
	}

	AVFrame *to_send = enc->sw_frame;
	if (enc->use_vaapi) {
		if (av_hwframe_get_buffer(enc->hw_frames_ctx, enc->hw_frame, 0) < 0) return false;
		if (av_hwframe_transfer_data(enc->hw_frame, enc->sw_frame, 0) < 0) return false;
		enc->hw_frame->pts = enc->sw_frame->pts;
		to_send = enc->hw_frame;
	}

	if (avcodec_send_frame(enc->ctx, to_send) < 0) return false;
	return drain_packets(enc, pts_ns, resource_id, flags, alpha, alpha_size);
}

bool np_encoder_push_bgra(struct np_encoder *enc, const uint8_t *bgra, int width, int height,
	                          int stride, uint64_t pts_ns, uint32_t resource_id,
	                          uint8_t flags) {
	if (!enc || !bgra || !resource_id || width < 2 || height < 2 || stride < width * 4)
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

	pthread_mutex_lock(&enc->lock);
	if (even_width != enc->requested_width || even_height != enc->requested_height) {
		enc->requested_width = even_width;
		enc->requested_height = even_height;
		enc->advertised_epoch++;
		enc->pending_force = true;
	}
	free(enc->pending_bgra); /* latest frame wins under encoder backpressure */
	enc->pending_bgra = copy;
	enc->pending_width = even_width;
	enc->pending_height = even_height;
	enc->pending_stride = (int)packed_stride;
	enc->pending_pts = pts_ns;
	enc->pending_resource_id = resource_id;
	enc->pending_flags = flags;
	enc->pending_epoch = enc->advertised_epoch;
	pthread_cond_signal(&enc->ready);
	pthread_mutex_unlock(&enc->lock);
	return true;
}
