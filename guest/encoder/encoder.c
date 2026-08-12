/*
 * H.264 encoder for NativePipe remote display.
 *
 * Prefers h264_vaapi when a VA-API device is available; falls back to libx264
 * (software) so GTK/cairo shm clients still encode on machines without
 * working hardware encode.
 */
#include "encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

struct np_encoder *np_encoder_create(int width, int height, np_encoder_output_fn out, void *user) {
	if (width < 2 || height < 2 || !out) return NULL;
	struct np_encoder *enc = calloc(1, sizeof(*enc));
	if (!enc) return NULL;
	enc->width = width & ~1;
	enc->height = height & ~1;
	enc->out = out;
	enc->user = user;
	enc->epoch = 1;
	enc->force_keyframe = true;
	if (!setup(enc)) {
		np_encoder_destroy(enc);
		return NULL;
	}
	return enc;
}

void np_encoder_destroy(struct np_encoder *enc) {
	if (!enc) return;
	teardown_codec(enc);
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
	return enc ? enc->epoch : 0;
}

void np_encoder_force_keyframe(struct np_encoder *enc) {
	if (enc) enc->force_keyframe = true;
}

static bool drain_packets(struct np_encoder *enc, uint64_t pts_ns) {
	for (;;) {
		int ret = avcodec_receive_packet(enc->ctx, enc->packet);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
		if (ret < 0) return false;
		enc->out(enc->user, enc->packet->data, (size_t)enc->packet->size, pts_ns, enc->epoch);
		av_packet_unref(enc->packet);
	}
}

bool np_encoder_push_bgra(struct np_encoder *enc, const uint8_t *bgra, int width, int height,
                          int stride, uint64_t pts_ns) {
	if (!enc || !bgra) return false;
	if ((width & ~1) != enc->width || (height & ~1) != enc->height) {
		if (!np_encoder_resize(enc, width, height)) return false;
	}
	if (av_frame_make_writable(enc->sw_frame) < 0) return false;
	const uint8_t *src_slices[4] = {bgra, NULL, NULL, NULL};
	int src_stride[4] = {stride, 0, 0, 0};
	sws_scale(enc->sws, src_slices, src_stride, 0, enc->height,
	          enc->sw_frame->data, enc->sw_frame->linesize);
	enc->sw_frame->pts++;
	if (enc->force_keyframe) {
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
	return drain_packets(enc, pts_ns);
}
