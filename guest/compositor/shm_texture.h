#ifndef NP_SHM_TEXTURE_H
#define NP_SHM_TEXTURE_H

/* The shared surface state still names this opaque type. RemotePipe copies
 * wl_shm bytes directly into the encoder and never creates a guest GPU mirror. */
struct np_shm_texture;
static inline void np_shm_texture_ref(struct np_shm_texture *texture)
{ (void)texture; }
static inline void np_shm_texture_unref(struct np_shm_texture *texture)
{ (void)texture; }

#endif
