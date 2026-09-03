#ifndef NP_VENUS_H
#define NP_VENUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Host half of the virtual GPU's 3D path.
///
/// Guest: Linux virtio_gpu.ko + Mesa VirGL (capsets 1/2) or Venus (capset 4).
/// Mesa serializes both Gallium and Vulkan work as SUBMIT_3D.
///
/// Host: this file + one virglrenderer instance. VirGL capsets use ANGLE's
/// OpenGL ES implementation on Metal; Venus uses MoltenVK. Both paths retain
/// their native Metal textures for direct host composition.

typedef struct np_venus np_venus;

enum {
	NP_VENUS_CAPSET_VENUS = 4,
	NP_VENUS_CAPSET_VIRGL = 1,
	NP_VENUS_CAPSET_VIRGL2 = 2,
};

typedef struct np_renderer_resource_3d {
	uint32_t resource_id;
	uint32_t target;
	uint32_t format;
	uint32_t bind;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t array_size;
	uint32_t last_level;
	uint32_t nr_samples;
	uint32_t flags;
} np_renderer_resource_3d;

typedef struct np_renderer_iovec {
	void *base;
	size_t length;
} np_renderer_iovec;

typedef struct np_renderer_box {
	uint32_t x, y, z;
	uint32_t width, height, depth;
} np_renderer_box;

/// Result of a fence wait. `user` is whatever was passed to `np_venus_submit`.
typedef void (*np_venus_fence_fn)(void *user, uint64_t fence_id, bool ok);

typedef struct np_venus_blob {
	uint32_t resource_id;
	uint64_t blob_id;
	uint32_t blob_flags;
	uint32_t map_info;
	void *pointer;
	uint64_t size;
} np_venus_blob;

/// Initializes the virglrenderer code linked into LightHouseVMHost.
/// Always returns an object; `np_venus_is_live` reports initialization failure
/// (for example, when the bundled ANGLE runtime cannot create a Metal display).
np_venus *np_venus_create(void);

/// Releases this VM's resources, contexts, renderer, and fence callbacks.
/// A VMHost owns exactly one renderer and exits with that VM.
void np_venus_destroy(np_venus *venus);

/// True when virglrenderer initialized and reported at least one 3D capset.
bool np_venus_is_live(const np_venus *venus);

/// Fills the Venus GET_CAPSET_INFO entry. Zeros if initialization failed.
void np_venus_capset_info(np_venus *venus, uint32_t *max_version, uint32_t *max_size);

/// Writes the capset blob. `buffer` must be at least `max_size` bytes.
/// Returns the number of bytes written, or 0 if Venus is not live.
uint32_t np_venus_fill_caps(np_venus *venus, uint32_t version, void *buffer, uint32_t buffer_size);

void np_renderer_capset_info(np_venus *venus, uint32_t capset_id,
                             uint32_t *max_version, uint32_t *max_size);
uint32_t np_renderer_fill_caps(np_venus *venus, uint32_t capset_id,
                              uint32_t version, void *buffer, uint32_t buffer_size);

int np_venus_context_create(np_venus *venus, uint32_t ctx_id, uint32_t capset_id,
                            const char *name);
void np_venus_context_destroy(np_venus *venus, uint32_t ctx_id);

int np_renderer_resource_create_3d(np_venus *venus,
                                   const np_renderer_resource_3d *resource);
int np_renderer_resource_attach_iov(np_venus *venus, uint32_t resource_id,
                                    const np_renderer_iovec *entries, uint32_t count);
void np_renderer_resource_detach_iov(np_venus *venus, uint32_t resource_id);
int np_renderer_transfer_3d(np_venus *venus, uint32_t resource_id, uint32_t ctx_id,
                            uint32_t level, uint32_t stride, uint32_t layer_stride,
                            const np_renderer_box *box, uint64_t offset,
                            bool from_host);

/// Registers a HOST3D blob with virglrenderer.
///
/// `blob_id == 0` is vkr shm (command ring). A non-zero `blob_id` names a
/// VkDeviceMemory vkr already allocated. On success `blob->pointer` is that
/// host mapping when the memory is CPU-mappable; DEVICE_LOCAL images remain
/// ordinary MTLHeap resources.
/// Returns the virglrenderer errno; compositor callers may ignore it.
int np_venus_create_blob(np_venus *venus, uint32_t ctx_id, np_venus_blob *blob);

void np_venus_unimport_blob(np_venus *venus, uint32_t resource_id);

/// Borrowed MTLTexture for a Venus image, or NULL.
/// `width`/`height`/`stride`/`virgl_format` come from the Wayland commit;
/// NativePipe retains either the exact exported VkImage texture or a zero-copy
/// texture view of Mesa WSI's exported linear buffer. The pointer remains valid
/// until the blob is unimported.
void *np_venus_metal_texture(np_venus *venus, uint32_t resource_id,
                             uint32_t width, uint32_t height,
                             uint32_t stride, uint32_t virgl_format);

int np_venus_attach(np_venus *venus, uint32_t ctx_id, uint32_t resource_id);
int np_venus_detach(np_venus *venus, uint32_t ctx_id, uint32_t resource_id);

/// Forwards a Venus command buffer. If `wants_fence` is true, `done` is called
/// when the GPU work retires (possibly on another thread). If false, `done`
/// is called before this returns. Returns non-zero if the submit itself failed.
///
/// `ring_idx` is the virtio-gpu ring the guest asked the fence to ride on.
/// Ring 0 is the CPU timeline and retires as soon as vkr decoded the stream;
/// ring N >= 1 is a VkQueue timeline and retires only after MoltenVK actually
/// finished the submitted work. Dropping this to 0 makes every fence lie,
/// which deadlocks Mesa's fence-feedback waits in the guest.
int np_venus_submit(np_venus *venus, uint32_t ctx_id, uint32_t ring_idx,
                    const void *payload, uint32_t byte_count,
                    bool wants_fence, uint64_t fence_id,
                    np_venus_fence_fn done, void *user);

#ifdef __cplusplus
}
#endif

#endif
