// virtio-gpu blob allocation: the guest end of the zero-copy path.
//
// A blob created here is host memory. The guest maps it and writes into it, and
// those stores land in the IOSurface the host will hand to CoreAnimation — so
// the memcpy out of a client's wl_shm pool is the only copy in the whole path,
// and it doubles as the transfer.

#ifndef NATIVEPIPE_BLOB_H
#define NATIVEPIPE_BLOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// The host page size. Ideally the host would advertise this rather than the
/// guest assuming it; until then, 16 KiB is safe on 4 KiB hosts as well.
#define NP_APERTURE_ALIGNMENT ((size_t)16384)

/// Row alignment the host's display pipeline requires, from
/// IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, …).
///
/// IOSurface will happily *store* an arbitrary bytesPerRow — it honours whatever
/// is asked for — but CoreAnimation cannot display a surface whose rows are not
/// aligned, and it fails by showing nothing rather than by complaining. A window
/// that is blank at some sizes and fine at others is this constant being
/// ignored: 480x320 works only because 480*4 happens to be a multiple of 128.
#define NP_ROW_ALIGNMENT ((size_t)128)

static inline size_t np_align_row(size_t stride) {
	return (stride + NP_ROW_ALIGNMENT - 1) & ~(NP_ROW_ALIGNMENT - 1);
}

struct np_blob {
	uint32_t bo_handle;
	/// The id the host knows this resource by. Goes into the frame message.
	uint32_t resource_id;
	size_t size;
	void *data;
};

/// Opens the render node and creates a venus context. Returns -1 on failure.
int np_blob_open(void);

void np_blob_close(int fd);

/// Allocates host memory of at least `size` bytes and maps it.
///
/// `size` is rounded up to NP_APERTURE_ALIGNMENT — the *host* page size, which
/// on Apple silicon is 16 KiB while this guest's pages are 4 KiB. The host's
/// mapMemory() requires host-page alignment of both offset and length, and the
/// guest's own drm_mm allocator hands out offsets with only 4 KiB granularity;
/// keeping every blob a multiple of 16 KiB keeps every offset one too, because
/// the arena starts at zero and never leaves a smaller hole.
///
/// `width`, `height` and `stride` describe the window this blob *is*. A HOST3D
/// blob is the window buffer — the same IOSurface the host composites — so a
/// compositor-created blob has to carry its shape. They travel in `blob_id`
/// (width occupies bits 63:48, so a Mesa Venus blob_id, a small counter, never
/// collides). Pass zeros for a linear blob; it is still an IOSurface, just not
/// yet shaped for `CALayer.contents`.
bool np_blob_create(int fd, size_t size, uint32_t width, uint32_t height,
                    uint32_t stride, struct np_blob *out);

/// Packs surface geometry into the 64-bit blob_id. Mirrored by
/// VirtioGPU.BlobGeometry on the host.
static inline uint64_t np_blob_pack_geometry(uint32_t width, uint32_t height, uint32_t stride) {
	if (width == 0 || height == 0 || width > 0xffff || height > 0xffff) return 0;
	return ((uint64_t)width << 48) | ((uint64_t)height << 32) | (uint64_t)stride;
}

void np_blob_destroy(int fd, struct np_blob *blob);

#endif
