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
/// Guest: Linux virtio_gpu.ko + Mesa Venus (capset 4). We do not implement
/// or patch Venus. Mesa serializes Vulkan as SUBMIT_3D.
///
/// Host: this file + virglrenderer (vkr) + MoltenVK. We advertise capset 4,
/// create vkr contexts, and hand SUBMIT_3D to virglrenderer. MoltenVK is
/// the ICD. A window buffer is a separate host object (IOSurface) created
/// by the virtio-gpu device for the compositor; device-local Vulkan memory
/// stays inside MoltenVK.

typedef struct np_venus np_venus;

enum {
	NP_VENUS_CAPSET_VENUS = 4,
};

/// Result of a fence wait. `user` is whatever was passed to `np_venus_submit`.
typedef void (*np_venus_fence_fn)(void *user, uint64_t fence_id, bool ok);

typedef struct np_venus_blob {
	uint32_t resource_id;
	uint64_t blob_id;
	uint32_t blob_flags;
	void *pointer;
	uint64_t size;
	uint32_t width;
	uint32_t height;
	uint32_t bytes_per_row;
	/// CFTypeRef / IOSurfaceRef. Retained for the life of the blob.
	void *iosurface;
} np_venus_blob;

/// Opens the renderer. Always returns an object: without virglrenderer the
/// device still maps blobs for the compositor, it just does not advertise a
/// Venus capset, so Mesa will not try to start one.
np_venus *np_venus_create(void);

void np_venus_destroy(np_venus *venus);

/// True when virglrenderer initialised and reported a Venus capset. That is
/// what GET_CAPSET_INFO advertises, and what makes guest Mesa pick Venus.
bool np_venus_is_live(const np_venus *venus);

/// Fills GET_CAPSET_INFO from virglrenderer. Zeros if the library is absent,
/// so Mesa sees no Venus and stays on software.
void np_venus_capset_info(np_venus *venus, uint32_t *max_version, uint32_t *max_size);

/// Writes the capset blob. `buffer` must be at least `max_size` bytes.
/// Returns the number of bytes written, or 0 if Venus is not live.
uint32_t np_venus_fill_caps(np_venus *venus, uint32_t version, void *buffer, uint32_t buffer_size);

int np_venus_context_create(np_venus *venus, uint32_t ctx_id, uint32_t capset_id,
                            const char *name);
void np_venus_context_destroy(np_venus *venus, uint32_t ctx_id);

/// Registers a HOST3D blob with virglrenderer.
///
/// Compositor (guest vmpipe-wayland): `iosurface` is the window buffer
/// already allocated on the host; vkr has no VkDeviceMemory for this
/// `blob_id` and create_blob is allowed to fail.
///
/// Mesa Venus: `iosurface` is NULL. `blob_id == 0` is vkr shm (command
/// ring). A non-zero `blob_id` names a VkDeviceMemory vkr already
/// allocated. On success `blob->pointer` is that host mapping.
/// Returns the virglrenderer errno; compositor callers may ignore it.
int np_venus_create_blob(np_venus *venus, uint32_t ctx_id, np_venus_blob *blob);

void np_venus_unimport_blob(np_venus *venus, uint32_t resource_id);

/// CFRetain'd MTLTexture for a Venus swapchain image, or NULL.
/// `width`/`height`/`stride`/`virgl_format` come from the Wayland commit;
/// UTM's create_handle_for_scanout builds an MTLTexture view of the MTLHeap.
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

/// Pump the render-server / ring threads. Call while waiting for a
/// vkAllocateMemory object to exist before CREATE_BLOB.
void np_venus_poll(np_venus *venus);

#ifdef __cplusplus
}
#endif

#endif
