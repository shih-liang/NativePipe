/* Direct VA-API AV1: one input surface, two alternating reconstructed surfaces,
 * no frame reordering. libva is optional at runtime, including on CPU-only hosts.
 * Packed headers implement AV1 Main/8-bit/420, one tile, LAST-only prediction.
 * VA calls and bitstream definitions: libva va_enc_av1.h, AV1 specification 5.5/5.9.
 */
#include "encoder_vaapi.h"
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_av1.h>
#include <libyuv/convert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VA_FUNCTIONS(X) \
 X(vaInitialize) X(vaTerminate) X(vaMaxNumEntrypoints) X(vaQueryConfigEntrypoints) X(vaGetConfigAttributes) \
 X(vaCreateConfig) X(vaDestroyConfig) X(vaCreateSurfaces) X(vaDestroySurfaces) \
 X(vaCreateContext) X(vaDestroyContext) X(vaCreateImage) X(vaDestroyImage) X(vaPutImage) \
 X(vaCreateBuffer) X(vaDestroyBuffer) X(vaMapBuffer) X(vaUnmapBuffer) \
 X(vaBeginPicture) X(vaRenderPicture) X(vaEndPicture) X(vaSyncSurface2)
struct np_av1_vaapi {
    void *library, *drm_library;
    VADisplay display;
    int fd, width, height;
    bool initialized;
    VAConfigID config;
    VAContextID context;
    VASurfaceID surfaces[3];
    VAImage image;
    VABufferID coded;
    unsigned frame, slot, obu_bytes, tx_mode;
#define DECLARE(name) __typeof__(&name) name;
    VA_FUNCTIONS(DECLARE)
#undef DECLARE
};

void np_av1_vaapi_destroy(struct np_av1_vaapi *e) {
    if (!e) return;
    if (e->display && e->initialized) {
        if (e->coded != VA_INVALID_ID) e->vaDestroyBuffer(e->display, e->coded);
        if (e->image.image_id != VA_INVALID_ID) e->vaDestroyImage(e->display, e->image.image_id);
        if (e->context != VA_INVALID_ID) e->vaDestroyContext(e->display, e->context);
        for (int i = 0; i < 3; i++)
            if (e->surfaces[i] != VA_INVALID_SURFACE) e->vaDestroySurfaces(e->display, &e->surfaces[i], 1);
        if (e->config != VA_INVALID_ID) e->vaDestroyConfig(e->display, e->config);
        e->vaTerminate(e->display);
    }
    if (e->fd >= 0) close(e->fd);
    if (e->drm_library) dlclose(e->drm_library);
    if (e->library) dlclose(e->library);
    free(e);
}

static struct np_av1_vaapi *open_device(const char *device, int width, int height) {
    struct np_av1_vaapi *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->fd = -1; e->width = width; e->height = height;
    e->config = e->context = e->coded = e->image.image_id = VA_INVALID_ID;
    for (int i = 0; i < 3; i++) e->surfaces[i] = VA_INVALID_SURFACE;
    e->library = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
    e->drm_library = dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!e->library || !e->drm_library) goto fail;
#define LOAD(name) if (!(e->name = dlsym(e->library, #name))) goto fail;
    VA_FUNCTIONS(LOAD)
#undef LOAD
    __typeof__(&vaGetDisplayDRM) get_display = dlsym(e->drm_library, "vaGetDisplayDRM");
    if (!get_display || (e->fd = open(device, O_RDWR | O_CLOEXEC)) < 0) goto fail;
    e->display = get_display(e->fd);
    int major, minor;
    if (!e->display || e->vaInitialize(e->display, &major, &minor)) goto fail;
    e->initialized = true;
    int capacity = e->vaMaxNumEntrypoints(e->display), count = 0;
    if (capacity < 1 || capacity > 256) goto fail;
    VAEntrypoint *entries = calloc(capacity, sizeof(*entries));
    if (!entries) goto fail;
    if (e->vaQueryConfigEntrypoints(e->display, VAProfileAV1Profile0, entries, &count) ||
        count < 0 || count > capacity) { free(entries); goto fail; }
    VAEntrypoint entry = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i] == VAEntrypointEncSliceLP) { entry = entries[i]; break; }
        if (entries[i] == VAEntrypointEncSlice) entry = entries[i];
    }
    free(entries);
    if (!entry) goto fail;
    VAConfigAttrib attrs[] = {{.type=VAConfigAttribRTFormat}, {.type=VAConfigAttribRateControl},
        {.type=VAConfigAttribEncPackedHeaders}, {.type=VAConfigAttribEncAV1Ext2},
        {.type=VAConfigAttribEncAV1}, {.type=VAConfigAttribEncAV1Ext1}};
    if (e->vaGetConfigAttributes(e->display, VAProfileAV1Profile0, entry, attrs, 6)) goto fail;
    unsigned packed = VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE;
    if (attrs[0].value == VA_ATTRIB_NOT_SUPPORTED || !(attrs[0].value & VA_RT_FORMAT_YUV420) ||
        attrs[1].value == VA_ATTRIB_NOT_SUPPORTED || !(attrs[1].value & VA_RC_CQP) ||
        attrs[2].value == VA_ATTRIB_NOT_SUPPORTED || (attrs[2].value & packed) != packed) goto fail;
    e->obu_bytes = 4; e->tx_mode = 2;
    if (attrs[3].value != VA_ATTRIB_NOT_SUPPORTED) {
        VAConfigAttribValEncAV1Ext2 ext = {.value=attrs[3].value};
        e->obu_bytes = ext.bits.obu_size_bytes_minus1 + 1;
        e->tx_mode = (ext.bits.tx_mode_support & 4) ? 2 : (ext.bits.tx_mode_support & 2) ? 1 : 0;
        if (!e->tx_mode) goto fail;
    }
    // This backend disables optional coding tools. Do not submit a sequence
    // that contradicts a driver's mandatory feature or interpolation mode.
    if (attrs[4].value != VA_ATTRIB_NOT_SUPPORTED) {
        for (unsigned shift = 0; shift < 28; shift += 2)
            if (((attrs[4].value >> shift) & 3) == VA_FEATURE_REQUIRED) goto fail;
    }
    if (attrs[5].value != VA_ATTRIB_NOT_SUPPORTED) {
        VAConfigAttribValEncAV1Ext1 ext = {.value=attrs[5].value};
        if (!(ext.bits.interpolation_filter & 1)) goto fail;
    }
    attrs[0].value = VA_RT_FORMAT_YUV420;
    attrs[1].value = VA_RC_CQP; attrs[2].value = packed;
    if (e->vaCreateConfig(e->display, VAProfileAV1Profile0, entry, attrs, 3, &e->config)) goto fail;
    VASurfaceAttrib format = {.type=VASurfaceAttribPixelFormat, .flags=VA_SURFACE_ATTRIB_SETTABLE,
        .value={.type=VAGenericValueTypeInteger, .value.i=VA_FOURCC_NV12}};
    int sw = (width + 63) & ~63, sh = (height + 63) & ~63;
    if (e->vaCreateSurfaces(e->display, VA_RT_FORMAT_YUV420, sw, sh, e->surfaces, 3, &format, 1) ||
        e->vaCreateContext(e->display, e->config, sw, sh, VA_PROGRESSIVE, e->surfaces, 3, &e->context)) goto fail;
    VAImageFormat image = {.fourcc=VA_FOURCC_NV12, .byte_order=VA_LSB_FIRST, .bits_per_pixel=12};
    if (e->vaCreateImage(e->display, &image, width, height, &e->image)) goto fail;
    unsigned bytes = (unsigned)width * height * 3 / 2 + 65536;
    if (e->vaCreateBuffer(e->display, e->context, VAEncCodedBufferType, bytes, 1, NULL, &e->coded)) goto fail;
    return e;
fail:
    np_av1_vaapi_destroy(e);
    return NULL;
}

bool np_av1_vaapi_supports_size(int width, int height) {
    // A single tile is limited by AV1's maximum tile width and area. Larger
    // surfaces use libaom's automatic tiling rather than unsupported headers.
    return width >= 2 && height >= 2 && width <= 4096 && height <= 4096 &&
        ((unsigned)width + 63) / 64 * (((unsigned)height + 63) / 64) <= 2304;
}

struct np_av1_vaapi *np_av1_vaapi_create(int width, int height) {
    if (!np_av1_vaapi_supports_size(width, height)) return NULL;
    for (int i = 128; i < 144; i++) {
        char device[64]; snprintf(device, sizeof(device), "/dev/dri/renderD%d", i);
        if (access(device, R_OK | W_OK)) continue;
        struct np_av1_vaapi *e = open_device(device, width, height);
        if (e) { fprintf(stderr, "[encoder] AV1 VA-API %s %dx%d\n", device, width, height); return e; }
    }
    return NULL;
}

struct bits { uint8_t data[256]; unsigned count; };
static void put(struct bits *b, unsigned value, unsigned n) {
    for (unsigned i = n; i; i--, b->count++)
        b->data[b->count / 8] |= ((value >> (i - 1)) & 1) << (7 - b->count % 8);
}
static unsigned bit_width(unsigned value) { unsigned n = 0; do { n++; } while (value >>= 1); return n; }
static void end_bits(struct bits *b) { put(b, 1, 1); while (b->count % 8) put(b, 0, 1); }
static size_t obu(uint8_t *out, unsigned type, unsigned length_bytes, const struct bits *b) {
    size_t bytes = (b->count + 7) / 8, value = bytes;
    out[0] = (type << 3) | 2;
    for (unsigned i = 0; i < length_bytes; i++) {
        out[i + 1] = (value & 127) | (i + 1 < length_bytes ? 128 : 0); value >>= 7;
    }
    if (value) return 0;
    memcpy(out + 1 + length_bytes, b->data, bytes);
    return bytes + 1 + length_bytes;
}
static size_t sequence_header(struct np_av1_vaapi *e, uint8_t *out) {
    struct bits b = {0};
    put(&b, 0, 3); put(&b, 0, 1); put(&b, 0, 1); // profile, still, reduced
    put(&b, 0, 1); put(&b, 0, 1); put(&b, 0, 5); // timing, initial delay, op count
    put(&b, 0, 12); put(&b, 13, 5); put(&b, 0, 1); // idc, level 5.1, main tier
    unsigned w = bit_width(e->width - 1), h = bit_width(e->height - 1);
    put(&b, w - 1, 4); put(&b, h - 1, 4);
    put(&b, e->width - 1, w); put(&b, e->height - 1, h);
    put(&b, 0, 1); // frame ids
    put(&b, 0, 3); // 64x64 superblocks, filter intra, intra edge filter
    put(&b, 0, 5); // interintra, masked compound, warped motion, dual filter, order hint
    put(&b, 0, 1); put(&b, 0, 1); // choose / force screen content tools
    put(&b, 0, 3); // superres, CDEF, restoration
    put(&b, 0, 1); put(&b, 0, 1); put(&b, 1, 1); // 8-bit, not mono, color description
    put(&b, 1, 8); put(&b, 13, 8); put(&b, 6, 8); // BT709 primaries, sRGB transfer, BT601 matrix
    put(&b, 0, 1); put(&b, 0, 2); put(&b, 0, 1); // limited range, chroma position, shared UV delta
    put(&b, 0, 1); end_bits(&b); // no grain
    return obu(out, 1, e->obu_bytes, &b);
}
static size_t picture_header(struct np_av1_vaapi *e, bool key, unsigned slot, uint8_t *out) {
    struct bits b = {0};
    put(&b, 0, 1); put(&b, key ? 0 : 1, 2); put(&b, 1, 1); // existing, type, show
    if (!key) put(&b, 0, 1); // non-error-resilient inter frame
    put(&b, 1, 1); put(&b, 0, 1); // disable CDF update, no size override
    if (!key) { put(&b, 7, 3); put(&b, 1u << slot, 8); }
    if (!key) for (unsigned i = 0; i < 7; i++) put(&b, e->slot, 3);
    put(&b, 0, 1); // render dimensions equal coded dimensions
    if (!key) { put(&b, 0, 1); put(&b, 0, 1); put(&b, 0, 2); put(&b, 0, 1); }
    put(&b, 1, 1); // uniform tiles: exactly one tile
    if (e->width > 64) put(&b, 0, 1);
    if (e->height > 64) put(&b, 0, 1);
    put(&b, 116, 8); put(&b, 0, 3); put(&b, 0, 1); // qindex, Y/U deltas, qmatrix
    put(&b, 0, 1); put(&b, 0, 1); // segmentation, delta q
    put(&b, 0, 12); put(&b, 0, 3); put(&b, 0, 1); // loop filters, sharpness, ref deltas
    put(&b, e->tx_mode == 2, 1);
    if (!key) put(&b, 0, 1); // single reference prediction
    put(&b, 1, 1); // reduced transform set
    if (!key) put(&b, 0, 7); // identity global motion
    end_bits(&b);
    return obu(out, 3, e->obu_bytes, &b);
}
static bool buffer(struct np_av1_vaapi *e, VABufferType type, const void *data, size_t size,
    VABufferID *buffers, unsigned *count) {
    if (*count >= 8) return false;
    if (e->vaCreateBuffer(e->display, e->context, type, size, 1, (void *)data, &buffers[*count])) return false;
    (*count)++; return true;
}
static bool packed_header(struct np_av1_vaapi *e, unsigned type, const uint8_t *data, size_t size,
    VABufferID *buffers, unsigned *count) {
    VAEncPackedHeaderParameterBuffer p = {.type=type, .bit_length=size * 8, .has_emulation_bytes=1};
    return size && buffer(e, VAEncPackedHeaderParameterBufferType, &p, sizeof(p), buffers, count) &&
        buffer(e, VAEncPackedHeaderDataBufferType, data, size, buffers, count);
}

bool np_av1_vaapi_encode(struct np_av1_vaapi *e, const uint8_t *planes[3], const int strides[3],
    bool key, uint8_t **packet, size_t *size) {
    *packet = NULL; *size = 0;
    key |= e->frame % 120 == 0;
    unsigned slot = key ? 0 : !e->slot;
    uint8_t *mapped;
    if (e->image.format.fourcc != VA_FOURCC_NV12 || e->image.num_planes != 2 ||
        e->vaMapBuffer(e->display, e->image.buf, (void **)&mapped)) return false;
    int conversion = I420ToNV12(planes[0], strides[0], planes[1], strides[1], planes[2], strides[2],
        mapped + e->image.offsets[0], e->image.pitches[0], mapped + e->image.offsets[1],
        e->image.pitches[1], e->width, e->height);
    e->vaUnmapBuffer(e->display, e->image.buf);
    if (conversion || e->vaPutImage(e->display, e->surfaces[0], e->image.image_id,
        0, 0, e->width, e->height, 0, 0, e->width, e->height)) return false;
    VAEncSequenceParameterBufferAV1 seq = {.seq_profile=0, .seq_level_idx=13, .intra_period=120, .ip_period=1};
    seq.seq_fields.bits.subsampling_x = seq.seq_fields.bits.subsampling_y = 1;
    VAEncPictureParameterBufferAV1 pic = {0};
    pic.frame_width_minus_1 = e->width - 1; pic.frame_height_minus_1 = e->height - 1;
    pic.reconstructed_frame = e->surfaces[1 + slot]; pic.coded_buf = e->coded;
    for (unsigned i = 0; i < 8; i++) pic.reference_frames[i] = VA_INVALID_SURFACE;
    if (!key) {
        pic.reference_frames[e->slot] = e->surfaces[1 + e->slot];
        for (unsigned i = 0; i < 7; i++) pic.ref_frame_idx[i] = e->slot;
        pic.ref_frame_ctrl_l0.fields.search_idx0 = 1;
    }
    pic.primary_ref_frame = 7; pic.refresh_frame_flags = key ? 255 : 1u << slot;
    pic.picture_flags.bits.frame_type = key ? 0 : 1;
    pic.picture_flags.bits.error_resilient_mode = key;
    pic.picture_flags.bits.disable_cdf_update = 1;
    pic.picture_flags.bits.disable_frame_end_update_cdf = 1;
    pic.picture_flags.bits.reduced_tx_set = 1;
    pic.base_qindex = 116;
    pic.tile_cols = pic.tile_rows = 1;
    pic.width_in_sbs_minus_1[0] = (e->width + 63) / 64 - 1;
    pic.height_in_sbs_minus_1[0] = (e->height + 63) / 64 - 1;
    pic.mode_control_flags.bits.tx_mode = e->tx_mode;
    pic.tile_group_obu_hdr_info.bits.obu_has_size_field = 1;
    VAEncTileGroupBufferAV1 tile = {0};
    uint8_t sequence[300], picture[300];
    size_t seq_size = sequence_header(e, sequence), pic_size = picture_header(e, key, slot, picture);
    VABufferID buffers[8]; unsigned count = 0;
    bool ok = (!key || (buffer(e, VAEncSequenceParameterBufferType, &seq, sizeof(seq), buffers, &count) &&
        packed_header(e, VAEncPackedHeaderSequence, sequence, seq_size, buffers, &count))) &&
        buffer(e, VAEncPictureParameterBufferType, &pic, sizeof(pic), buffers, &count) &&
        packed_header(e, VAEncPackedHeaderPicture, picture, pic_size, buffers, &count) &&
        buffer(e, VAEncSliceParameterBufferType, &tile, sizeof(tile), buffers, &count);
    bool began = false;
    if (ok) { began = e->vaBeginPicture(e->display, e->context, e->surfaces[0]) == VA_STATUS_SUCCESS; ok = began; }
    if (ok) ok = e->vaRenderPicture(e->display, e->context, buffers, count) == VA_STATUS_SUCCESS;
    if (began) ok = e->vaEndPicture(e->display, e->context) == VA_STATUS_SUCCESS && ok;
    // Input/reconstruction surfaces and uploaded pixels remain owned until the
    // GPU finishes. A timeout fails this backend, never recycles an in-use slot.
    if (began) ok = e->vaSyncSurface2(e->display, e->surfaces[0], 500000000ull) == VA_STATUS_SUCCESS && ok;
    for (unsigned i = 0; i < count; i++) e->vaDestroyBuffer(e->display, buffers[i]);
    if (!ok) return false;
    VACodedBufferSegment *segment;
    if (e->vaMapBuffer(e->display, e->coded, (void **)&segment)) return false;
    for (VACodedBufferSegment *s = segment; s; s = s->next) {
        if (s->bit_offset || s->status & (VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK | VA_CODED_BUF_STATUS_BAD_BITSTREAM) ||
            s->size > 32u * 1024u * 1024u - *size) { ok = false; break; }
        *size += s->size;
    }
    if (ok && *size) {
        *packet = malloc(*size);
        if (!*packet) ok = false;
        else {
            size_t offset = 0;
            for (VACodedBufferSegment *s = segment; s; s = s->next) {
                memcpy(*packet + offset, s->buf, s->size); offset += s->size;
            }
        }
    } else ok = false;
    e->vaUnmapBuffer(e->display, e->coded);
    if (!ok) { free(*packet); *packet = NULL; *size = 0; return false; }
    e->slot = slot; e->frame++;
    return true;
}
