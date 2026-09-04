#ifndef NP_DMABUF_EGL_H
#define NP_DMABUF_EGL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct np_egl_importer;
struct np_egl_buffer;

struct np_dmabuf_format_modifier {
	uint32_t format;
	uint64_t modifier;
};

struct np_egl_importer *np_egl_importer_create(int drm_fd);
void np_egl_importer_destroy(struct np_egl_importer *importer);

const struct np_dmabuf_format_modifier *np_egl_importer_formats(
	const struct np_egl_importer *importer, size_t *count);
bool np_egl_importer_supports(
	const struct np_egl_importer *importer, uint32_t format, uint64_t modifier);

struct np_egl_buffer *np_egl_buffer_import(
	struct np_egl_importer *importer, int fd,
	int32_t width, int32_t height, uint32_t format,
	uint32_t offset, uint32_t stride, uint64_t modifier);
void np_egl_buffer_destroy(struct np_egl_buffer *buffer);

/* Returns a buffer-owned, tightly packed BGRA image. It remains valid until
 * the next read of this buffer or until the buffer is destroyed. */
bool np_egl_buffer_read_bgra(
	struct np_egl_buffer *buffer, bool force_opaque,
	const unsigned char **pixels, int32_t *stride);

#endif
