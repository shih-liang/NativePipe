#include "nativepipe_av1.h"
#include <nativepipe_codec_build.h>
#include <dav1d/dav1d.h>
#include <libyuv/convert_argb.h>
#include <libyuv/row.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

__attribute__((used)) static const char codec_build_id[] = NP_CODEC_BUILD_ID;

struct np_av1_decoder { Dav1dContext *context; };

int np_av1_configuration(const uint8_t *bytes, size_t size, int width, int height, uint8_t out[4]) {
    Dav1dSequenceHeader h;
    if (!bytes || !out || size > 32u * 1024u * 1024u || width < 1 || height < 1 ||
        (uint64_t)width * height > 16777216 || dav1d_parse_sequence_header(&h, bytes, size) ||
        h.profile != 0 || h.hbd != 0 || h.layout != DAV1D_PIXEL_LAYOUT_I420 ||
        h.max_width != width || h.max_height != height) return -1;
    out[0] = 0x81;
    out[1] = (h.operating_points[0].major_level - 2) * 4 + h.operating_points[0].minor_level;
    out[2] = (h.operating_points[0].tier << 7) | 0x0c | h.chr;
    out[3] = 0;
    return 0;
}

struct np_av1_decoder *np_av1_decoder_create(void) {
    struct np_av1_decoder *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.n_threads = 2;
    settings.max_frame_delay = 1;
    settings.frame_size_limit = 16777216;
    settings.strict_std_compliance = 1;
    if (dav1d_open(&d->context, &settings)) { free(d); return NULL; }
    return d;
}

void np_av1_decoder_destroy(struct np_av1_decoder *d) {
    if (d) { dav1d_close(&d->context); free(d); }
}

CVPixelBufferRef np_av1_decoder_decode(struct np_av1_decoder *d, const uint8_t *bytes,
    size_t size, int width, int height) {
    if (!d || !bytes || !size || size > 32u * 1024u * 1024u || width < 1 || height < 1 ||
        (uint64_t)width * height > 16777216) return NULL;
    Dav1dData input = {0};
    uint8_t *copy = dav1d_data_create(&input, size);
    if (!copy) return NULL;
    memcpy(copy, bytes, size);
    int result = dav1d_send_data(d->context, &input);
    dav1d_data_unref(&input);
    if (result) return NULL;
    Dav1dPicture p = {0};
    if (dav1d_get_picture(d->context, &p)) return NULL;
    CVPixelBufferRef buffer = NULL;
    if (p.p.w != width || p.p.h != height || p.p.bpc != 8 ||
        p.p.layout != DAV1D_PIXEL_LAYOUT_I420 || p.stride[0] > INT_MAX || p.stride[1] > INT_MAX) goto done;
    CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
    const void *values[] = {empty};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CVReturn status = CVPixelBufferCreate(NULL, width, height, kCVPixelFormatType_32BGRA, attrs, &buffer);
    CFRelease(attrs); CFRelease(empty);
    if (status != kCVReturnSuccess) { buffer = NULL; goto done; }
    if (CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess) {
        CVPixelBufferRelease(buffer); buffer = NULL; goto done;
    }
    const struct YuvConstants *matrix;
    if (p.seq_hdr->mtrx == DAV1D_MC_BT709)
        matrix = p.seq_hdr->color_range ? &kYuvF709Constants : &kYuvH709Constants;
    else if (p.seq_hdr->mtrx == DAV1D_MC_BT2020_NCL)
        matrix = p.seq_hdr->color_range ? &kYuvV2020Constants : &kYuv2020Constants;
    else
        matrix = p.seq_hdr->color_range ? &kYuvJPEGConstants : &kYuvI601Constants;
    result = I420ToARGBMatrix(p.data[0], (int)p.stride[0], p.data[1], (int)p.stride[1],
        p.data[2], (int)p.stride[1], CVPixelBufferGetBaseAddress(buffer),
        (int)CVPixelBufferGetBytesPerRow(buffer), matrix, width, height);
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    if (result) { CVPixelBufferRelease(buffer); buffer = NULL; }
    // A second picture would have no resource ID. Do not silently mislabel it.
    Dav1dPicture extra = {0};
    if (!dav1d_get_picture(d->context, &extra)) {
        dav1d_picture_unref(&extra);
        if (buffer) CVPixelBufferRelease(buffer);
        buffer = NULL;
    }
done:
    dav1d_picture_unref(&p);
    return buffer;
}
