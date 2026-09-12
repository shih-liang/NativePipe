/* Shared NPEN media frame header (guest C + host Swift MediaWire). */
#ifndef NATIVEPIPE_MEDIA_H
#define NATIVEPIPE_MEDIA_H

#include <stdint.h>

#define NP_MEDIA_PORT 1026
#define NP_MEDIA_MAGIC "NPEN"
#define NP_MEDIA_VERSION 2
#define NP_MEDIA_HEADER_SIZE 36
#define NP_MEDIA_CODEC_H264 1
#define NP_MEDIA_CODEC_ALPHA_RLE 2
#define NP_MEDIA_CODEC_AV1 3
#define NP_MEDIA_FLAG_HAS_ALPHA 1
#define NP_MEDIA_FLAG_HARDWARE 4

struct np_media_header {
	char magic[4];       /* "NPEN" */
	uint8_t version;     /* 2 */
	uint8_t codec;       /* NP_MEDIA_CODEC_* */
	uint8_t flags;
	uint8_t pad;
	uint32_t surface_id; /* little-endian on wire */
	uint32_t resource_id;
	uint16_t width;
	uint16_t height;
	uint64_t pts_ns;
	uint32_t length;
	uint16_t bitstream_epoch;
	uint16_t reserved;
};

#endif
