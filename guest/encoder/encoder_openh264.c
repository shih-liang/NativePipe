/*
 * Self-contained H.264 encoder used by the static RemotePipe build.
 *
 * The regular remote build keeps FFmpeg so it can use VA-API.  This backend
 * deliberately depends only on OpenH264: it can therefore be linked into a
 * musl executable and copied to a Linux host without installing codec or
 * Wayland libraries there.
 */
#include "encoder.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wels/codec_api.h>

struct np_encoder {
	int width;
	int height;
	uint16_t epoch;
	np_encoder_output_fn out;
	void *user;

	ISVCEncoder *codec;
	uint8_t *i420;
	bool force_keyframe;

	/* Wayland submits only the newest frame; encoding runs off its event loop. */
	pthread_t worker;
	pthread_mutex_t lock;
	pthread_cond_t ready;
	bool worker_started;
	bool stopping;
	uint8_t *pending_bgra;
	int pending_width;
	int pending_height;
	int pending_stride;
	uint64_t pending_pts;
	uint16_t pending_epoch;
	bool pending_force;
	int requested_width;
	int requested_height;
	uint16_t advertised_epoch;
};

static uint8_t clamp_u8(int value) {
	if (value < 0) return 0;
	if (value > 255) return 255;
	return (uint8_t)value;
}

/* BT.709 limited-range conversion matching the VUI written below. */
static void bgra_to_i420(uint8_t *dst, const uint8_t *src, int width, int height,
	                    int stride) {
	uint8_t *y_plane = dst;
	uint8_t *u_plane = y_plane + (size_t)width * (size_t)height;
	uint8_t *v_plane = u_plane + ((size_t)width * (size_t)height) / 4;

	for (int y = 0; y < height; y += 2) {
		const uint8_t *rows[2] = {
			src + (size_t)y * (size_t)stride,
			src + (size_t)(y + 1) * (size_t)stride,
		};
		for (int x = 0; x < width; x += 2) {
			int red = 0;
			int green = 0;
			int blue = 0;
			for (int dy = 0; dy < 2; dy++) {
				for (int dx = 0; dx < 2; dx++) {
					const uint8_t *pixel = rows[dy] + (size_t)(x + dx) * 4;
					int b = pixel[0];
					int g = pixel[1];
					int r = pixel[2];
					red += r;
					green += g;
					blue += b;
					int luma = ((47 * r + 157 * g + 16 * b + 128) >> 8) + 16;
					y_plane[(size_t)(y + dy) * (size_t)width + (size_t)(x + dx)] =
						clamp_u8(luma);
				}
			}
			red = (red + 2) >> 2;
			green = (green + 2) >> 2;
			blue = (blue + 2) >> 2;
			size_t chroma = (size_t)(y / 2) * (size_t)(width / 2) + (size_t)(x / 2);
			u_plane[chroma] = clamp_u8(((-26 * red - 87 * green + 112 * blue + 128) >> 8) + 128);
			v_plane[chroma] = clamp_u8(((112 * red - 102 * green - 10 * blue + 128) >> 8) + 128);
		}
	}
}

static void teardown_codec(struct np_encoder *enc) {
	if (enc->codec) {
		(*enc->codec)->Uninitialize(enc->codec);
		WelsDestroySVCEncoder(enc->codec);
		enc->codec = NULL;
	}
	free(enc->i420);
	enc->i420 = NULL;
}

static bool setup_codec(struct np_encoder *enc) {
	if (WelsCreateSVCEncoder(&enc->codec) != cmResultSuccess || !enc->codec)
		return false;

	SEncParamExt params;
	memset(&params, 0, sizeof(params));
	if ((*enc->codec)->GetDefaultParams(enc->codec, &params) != cmResultSuccess)
		goto fail;

	const int bitrate = 8 * 1000 * 1000;
	params.iUsageType = SCREEN_CONTENT_REAL_TIME;
	params.iPicWidth = enc->width;
	params.iPicHeight = enc->height;
	params.iTargetBitrate = bitrate;
	params.iRCMode = RC_BITRATE_MODE;
	params.fMaxFrameRate = 60.0f;
	params.iTemporalLayerNum = 1;
	params.iSpatialLayerNum = 1;
	params.iComplexityMode = LOW_COMPLEXITY;
	params.uiIntraPeriod = 60;
	params.iNumRefFrame = 1;
	params.eSpsPpsIdStrategy = CONSTANT_ID;
	params.bEnableFrameSkip = true;
	params.iMaxBitrate = bitrate;
	params.iMultipleThreadIdc = 0;
	params.bEnableDenoise = false;
	params.bEnableBackgroundDetection = false;
	// OpenH264's screen-content path rejects adaptive quantization on several
	// static builds; leave it disabled rather than logging a warning per encoder.
	params.bEnableAdaptiveQuant = false;
	params.bEnableSceneChangeDetect = true;

	SSpatialLayerConfig *layer = &params.sSpatialLayers[0];
	layer->iVideoWidth = enc->width;
	layer->iVideoHeight = enc->height;
	layer->fFrameRate = 60.0f;
	layer->iSpatialBitrate = bitrate;
	layer->iMaxSpatialBitrate = bitrate;
	layer->sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
	layer->bVideoSignalTypePresent = true;
	layer->uiVideoFormat = VF_UNDEF;
	layer->bFullRange = false;
	layer->bColorDescriptionPresent = true;
	layer->uiColorPrimaries = CP_BT709;
	layer->uiTransferCharacteristics = TRC_BT709;
	layer->uiColorMatrix = CM_BT709;
	layer->bAspectRatioPresent = true;
	layer->eAspectRatio = ASP_1x1;

	if ((*enc->codec)->InitializeExt(enc->codec, &params) != cmResultSuccess)
		goto fail;
	int format = videoFormatI420;
	int trace = WELS_LOG_ERROR;
	if ((*enc->codec)->SetOption(enc->codec, ENCODER_OPTION_DATAFORMAT, &format) != cmResultSuccess)
		goto fail;
	(*enc->codec)->SetOption(enc->codec, ENCODER_OPTION_TRACE_LEVEL, &trace);

	size_t pixels = (size_t)enc->width * (size_t)enc->height;
	if (pixels > SIZE_MAX / 3 * 2) goto fail;
	enc->i420 = malloc(pixels + pixels / 2);
	if (!enc->i420) goto fail;
	enc->force_keyframe = true;
	fprintf(stderr, "[encoder] openh264-static %dx%d\n", enc->width, enc->height);
	return true;

fail:
	teardown_codec(enc);
	return false;
}

static bool emit_frame(struct np_encoder *enc, const SFrameBSInfo *info,
	                   uint64_t pts_ns) {
	if (info->eFrameType == videoFrameTypeSkip) return true;
	size_t total = 0;
	for (int i = 0; i < info->iLayerNum; i++) {
		const SLayerBSInfo *layer = &info->sLayerInfo[i];
		for (int n = 0; n < layer->iNalCount; n++) {
			if (layer->pNalLengthInByte[n] < 0 ||
			    total > SIZE_MAX - (size_t)layer->pNalLengthInByte[n])
				return false;
			total += (size_t)layer->pNalLengthInByte[n];
		}
	}
	if (total == 0 || total > UINT32_MAX) return false;

	uint8_t *annex_b = malloc(total);
	if (!annex_b) return false;
	size_t offset = 0;
	for (int i = 0; i < info->iLayerNum; i++) {
		const SLayerBSInfo *layer = &info->sLayerInfo[i];
		size_t layer_size = 0;
		for (int n = 0; n < layer->iNalCount; n++)
			layer_size += (size_t)layer->pNalLengthInByte[n];
		memcpy(annex_b + offset, layer->pBsBuf, layer_size);
		offset += layer_size;
	}
	enc->out(enc->user, annex_b, total, pts_ns, enc->epoch,
	         (uint16_t)enc->width, (uint16_t)enc->height);
	free(annex_b);
	return true;
}

static bool np_encoder_resize_internal(struct np_encoder *enc, int width, int height) {
	width &= ~1;
	height &= ~1;
	if (width < 2 || height < 2) return false;
	if (width == enc->width && height == enc->height) return true;
	teardown_codec(enc);
	enc->width = width;
	enc->height = height;
	enc->epoch++;
	if (!setup_codec(enc)) return false;
	fprintf(stderr, "[encoder] resized %dx%d epoch=%u\n",
	        enc->width, enc->height, enc->epoch);
	return true;
}

static bool encode_bgra_now(struct np_encoder *enc, const uint8_t *bgra,
	                        int width, int height, int stride, uint64_t pts_ns,
	                        uint16_t epoch, bool force) {
	if ((width & ~1) != enc->width || (height & ~1) != enc->height) {
		if (!np_encoder_resize_internal(enc, width, height)) return false;
	}
	enc->epoch = epoch;
	bgra_to_i420(enc->i420, bgra, enc->width, enc->height, stride);
	if (force || enc->force_keyframe) {
		if ((*enc->codec)->ForceIntraFrame(enc->codec, true) != cmResultSuccess)
			return false;
		enc->force_keyframe = false;
	}

	SFrameBSInfo info;
	SSourcePicture picture;
	memset(&info, 0, sizeof(info));
	memset(&picture, 0, sizeof(picture));
	size_t pixels = (size_t)enc->width * (size_t)enc->height;
	picture.iColorFormat = videoFormatI420;
	picture.iStride[0] = enc->width;
	picture.iStride[1] = enc->width / 2;
	picture.iStride[2] = enc->width / 2;
	picture.pData[0] = enc->i420;
	picture.pData[1] = enc->i420 + pixels;
	picture.pData[2] = enc->i420 + pixels + pixels / 4;
	picture.iPicWidth = enc->width;
	picture.iPicHeight = enc->height;
	picture.uiTimeStamp = (long long)(pts_ns / 1000000ULL);
	if ((*enc->codec)->EncodeFrame(enc->codec, &picture, &info) != cmResultSuccess)
		return false;
	return emit_frame(enc, &info, pts_ns);
}

static void *encoder_worker(void *argument) {
	struct np_encoder *enc = argument;
	for (;;) {
		pthread_mutex_lock(&enc->lock);
		while (!enc->stopping && !enc->pending_bgra)
			pthread_cond_wait(&enc->ready, &enc->lock);
		if (enc->stopping && !enc->pending_bgra) {
			pthread_mutex_unlock(&enc->lock);
			break;
		}
		uint8_t *pixels = enc->pending_bgra;
		int width = enc->pending_width;
		int height = enc->pending_height;
		int stride = enc->pending_stride;
		uint64_t pts = enc->pending_pts;
		uint16_t epoch = enc->pending_epoch;
		bool force = enc->pending_force;
		enc->pending_force = false;
		enc->pending_bgra = NULL;
		pthread_mutex_unlock(&enc->lock);

		if (!encode_bgra_now(enc, pixels, width, height, stride, pts, epoch, force))
			fprintf(stderr, "[encoder] frame encode failed\n");
		free(pixels);
	}
	return NULL;
}

struct np_encoder *np_encoder_create(int width, int height,
	                                 np_encoder_output_fn out, void *user) {
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
	pthread_mutex_init(&enc->lock, NULL);
	pthread_cond_init(&enc->ready, NULL);
	if (!setup_codec(enc)) {
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
	teardown_codec(enc);
	pthread_cond_destroy(&enc->ready);
	pthread_mutex_destroy(&enc->lock);
	free(enc);
}

bool np_encoder_resize(struct np_encoder *enc, int width, int height) {
	if (!enc) return false;
	return np_encoder_resize_internal(enc, width, height);
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

bool np_encoder_push_bgra(struct np_encoder *enc, const uint8_t *bgra,
	                      int width, int height, int stride, uint64_t pts_ns) {
	if (!enc || !bgra || width < 2 || height < 2 || stride < width * 4)
		return false;
	int even_width = width & ~1;
	int even_height = height & ~1;
	size_t packed_stride = (size_t)even_width * 4;
	if ((size_t)even_height > SIZE_MAX / packed_stride) return false;
	uint8_t *copy = malloc(packed_stride * (size_t)even_height);
	if (!copy) return false;
	for (int row = 0; row < even_height; row++)
		memcpy(copy + (size_t)row * packed_stride,
		       bgra + (size_t)row * (size_t)stride, packed_stride);

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
	enc->pending_epoch = enc->advertised_epoch;
	pthread_cond_signal(&enc->ready);
	pthread_mutex_unlock(&enc->lock);
	return true;
}
