/* Validate our hardware packed headers with dav1d, without requiring a GPU.
 * This proves header syntax only; real hardware must still produce valid tiles. */
#include "../../encoder/encoder_vaapi.c"
#include <assert.h>
#include <dav1d/dav1d.h>

int main(void) {
    assert(np_av1_vaapi_supports_size(3840, 2160));
    assert(np_av1_vaapi_supports_size(4096, 2304));
    assert(!np_av1_vaapi_supports_size(4098, 64));
    assert(!np_av1_vaapi_supports_size(2560, 3680)); // Fits in pixels, exceeds tile area after padding.
    const int dimensions[][2] = {{2, 2}, {64, 64}, {66, 66}, {1920, 1080}, {3840, 2160}, {4096, 2304}};
    for (unsigned d = 0; d < sizeof(dimensions) / sizeof(dimensions[0]); d++)
    for (unsigned key = 0; key <= 1; key++)
    for (unsigned tx = 1; tx <= 2; tx++) {
        struct np_av1_vaapi encoder = {.width=dimensions[d][0], .height=dimensions[d][1], .obu_bytes=4, .tx_mode=tx};
        uint8_t headers[600];
        size_t size = sequence_header(&encoder, headers);
        Dav1dSequenceHeader sequence;
        assert(dav1d_parse_sequence_header(&sequence, headers, size) == 0);
        assert(sequence.profile == 0 && sequence.hbd == 0 && sequence.layout == DAV1D_PIXEL_LAYOUT_I420);
        assert(sequence.max_width == encoder.width && sequence.max_height == encoder.height);
        size += picture_header(&encoder, key, !encoder.slot, headers + size);
        Dav1dContext *decoder;
        Dav1dSettings settings;
        dav1d_default_settings(&settings);
        settings.n_threads = settings.max_frame_delay = 1;
        settings.strict_std_compliance = 1;
        assert(dav1d_open(&decoder, &settings) == 0);
        Dav1dData packet = {0};
        uint8_t *copy = dav1d_data_create(&packet, size); assert(copy);
        memcpy(copy, headers, size);
        assert(dav1d_send_data(decoder, &packet) == 0);
        dav1d_data_unref(&packet);
        Dav1dPicture picture = {0};
        // Valid complete headers, deliberately no tile data / displayed frame.
        assert(dav1d_get_picture(decoder, &picture) == DAV1D_ERR(EAGAIN));
        dav1d_close(&decoder);
    }
    puts("VA-API AV1 packed sequence/frame headers parsed by dav1d: PASS");
}
