#define _GNU_SOURCE

#include "dmabuf_egl.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <gbm.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

struct np_egl_importer {
	int drm_fd;
	struct gbm_device *gbm;
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
	struct np_dmabuf_format_modifier *formats;
	size_t format_count;
	GLuint program;
	GLuint framebuffer;
	GLuint output_texture;
	GLint position_location;
	GLint texcoord_location;
	GLint sampler_location;
	GLint force_opaque_location;
	int32_t output_width;
	int32_t output_height;
};

struct np_egl_buffer {
	struct np_egl_importer *importer;
	EGLImageKHR image;
	GLuint texture;
	int32_t width;
	int32_t height;
	unsigned char *pixels;
	size_t pixels_size;
};

static bool extension_present(const char *extensions, const char *name)
{
	if (!extensions || !name || !name[0] || strchr(name, ' ')) return false;
	size_t length = strlen(name);
	const char *match = extensions;
	while ((match = strstr(match, name))) {
		if ((match == extensions || match[-1] == ' ') &&
		    (match[length] == '\0' || match[length] == ' '))
			return true;
		match += length;
	}
	return false;
}

static bool make_current(struct np_egl_importer *importer)
{
	return importer && importer->display != EGL_NO_DISPLAY &&
	       importer->context != EGL_NO_CONTEXT &&
	       eglMakeCurrent(importer->display, importer->surface,
	                      importer->surface, importer->context) == EGL_TRUE;
}

static GLuint compile_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	if (!shader) return 0;
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint compiled = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (compiled == GL_TRUE) return shader;
	char log[1024] = {0};
	glGetShaderInfoLog(shader, sizeof(log), NULL, log);
	fprintf(stderr, "[wayland] dma-buf readback shader failed: %s\n", log);
	glDeleteShader(shader);
	return 0;
}

static bool create_program(struct np_egl_importer *importer)
{
	static const char vertex_source[] =
		"attribute vec2 position;\n"
		"attribute vec2 texcoord;\n"
		"varying vec2 texture_coord;\n"
		"void main() {\n"
		"  gl_Position = vec4(position, 0.0, 1.0);\n"
		"  texture_coord = texcoord;\n"
		"}\n";
	static const char fragment_source[] =
		"precision mediump float;\n"
		"uniform sampler2D source_texture;\n"
		"uniform float force_opaque;\n"
		"varying vec2 texture_coord;\n"
		"void main() {\n"
		"  vec4 color = texture2D(source_texture, texture_coord);\n"
		"  gl_FragColor = vec4(color.b, color.g, color.r,\n"
		"                      mix(color.a, 1.0, force_opaque));\n"
		"}\n";
	GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
	GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
	if (!vertex || !fragment) {
		if (vertex) glDeleteShader(vertex);
		if (fragment) glDeleteShader(fragment);
		return false;
	}
	importer->program = glCreateProgram();
	glAttachShader(importer->program, vertex);
	glAttachShader(importer->program, fragment);
	glLinkProgram(importer->program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	GLint linked = GL_FALSE;
	glGetProgramiv(importer->program, GL_LINK_STATUS, &linked);
	if (linked != GL_TRUE) {
		char log[1024] = {0};
		glGetProgramInfoLog(importer->program, sizeof(log), NULL, log);
		fprintf(stderr, "[wayland] dma-buf readback program failed: %s\n", log);
		return false;
	}
	importer->position_location =
		glGetAttribLocation(importer->program, "position");
	importer->texcoord_location =
		glGetAttribLocation(importer->program, "texcoord");
	importer->sampler_location =
		glGetUniformLocation(importer->program, "source_texture");
	importer->force_opaque_location =
		glGetUniformLocation(importer->program, "force_opaque");
	return importer->position_location >= 0 && importer->texcoord_location >= 0 &&
	       importer->sampler_location >= 0 && importer->force_opaque_location >= 0;
}

static bool append_format(
	struct np_egl_importer *importer, uint32_t format, uint64_t modifier)
{
	for (size_t index = 0; index < importer->format_count; index++) {
		if (importer->formats[index].format == format &&
		    importer->formats[index].modifier == modifier)
			return true;
	}
	if (importer->format_count == SIZE_MAX / sizeof(*importer->formats))
		return false;
	size_t count = importer->format_count + 1;
	void *resized = realloc(importer->formats, count * sizeof(*importer->formats));
	if (!resized) return false;
	importer->formats = resized;
	importer->formats[importer->format_count] =
		(struct np_dmabuf_format_modifier){format, modifier};
	importer->format_count = count;
	return true;
}

static bool query_formats(struct np_egl_importer *importer)
{
	static const uint32_t formats[] = {
		DRM_FORMAT_ARGB8888,
		DRM_FORMAT_XRGB8888,
	};
	for (size_t format_index = 0;
	     format_index < sizeof(formats) / sizeof(formats[0]); format_index++) {
		EGLint count = 0;
		if (!importer->query_modifiers(
				importer->display, (EGLint)formats[format_index],
				0, NULL, NULL, &count) || count < 0 || count > 4096)
			return false;
		EGLuint64KHR *modifiers = count
			? calloc((size_t)count, sizeof(*modifiers)) : NULL;
		EGLBoolean *external_only = count
			? calloc((size_t)count, sizeof(*external_only)) : NULL;
		if (count && (!modifiers || !external_only)) {
			free(modifiers);
			free(external_only);
			return false;
		}
		EGLint written = 0;
		bool queried = importer->query_modifiers(
			importer->display, (EGLint)formats[format_index], count,
			modifiers, external_only, &written) == EGL_TRUE;
		if (!queried || written < 0 || written > count) {
			free(modifiers);
			free(external_only);
			return false;
		}
		for (EGLint index = 0; index < written; index++) {
			/* External-only images require samplerExternalOES. Linear buffers
			 * already have a cheaper, layout-defined CPU path. */
			if (external_only[index]) continue;
			if (!append_format(
					importer, formats[format_index], (uint64_t)modifiers[index])) {
				free(modifiers);
				free(external_only);
				return false;
			}
		}
		free(modifiers);
		free(external_only);
		/* LINEAR is safe to map using the protocol offset and pitch even when
		 * EGL reports it as external-only. Keep it last so GBM prefers a
		 * renderable native modifier. */
		if (!append_format(importer, formats[format_index], DRM_FORMAT_MOD_LINEAR))
			return false;
	}
	return importer->format_count != 0;
}

struct np_egl_importer *np_egl_importer_create(int drm_fd)
{
	if (drm_fd < 0) return NULL;
	struct np_egl_importer *importer = calloc(1, sizeof(*importer));
	if (!importer) return NULL;
	importer->drm_fd = -1;
	importer->display = EGL_NO_DISPLAY;
	importer->context = EGL_NO_CONTEXT;
	importer->surface = EGL_NO_SURFACE;
	importer->drm_fd = fcntl(drm_fd, F_DUPFD_CLOEXEC, 3);
	if (importer->drm_fd < 0) goto failed;
	importer->gbm = gbm_create_device(importer->drm_fd);
	if (!importer->gbm) goto failed;
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_platform_display) goto failed;
	importer->display = get_platform_display(
		EGL_PLATFORM_GBM_KHR, importer->gbm, NULL);
	EGLint major = 0, minor = 0;
	if (importer->display == EGL_NO_DISPLAY ||
	    !eglInitialize(importer->display, &major, &minor))
		goto failed;
	const char *egl_extensions = eglQueryString(importer->display, EGL_EXTENSIONS);
	if (!extension_present(egl_extensions, "EGL_EXT_image_dma_buf_import") ||
	    !extension_present(egl_extensions,
	                       "EGL_EXT_image_dma_buf_import_modifiers"))
		goto failed;
	if (!eglBindAPI(EGL_OPENGL_ES_API)) goto failed;
	static const EGLint config_attributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_NONE,
	};
	EGLConfig config = NULL;
	EGLint config_count = 0;
	if (!eglChooseConfig(importer->display, config_attributes,
	                     &config, 1, &config_count) || config_count < 1)
		goto failed;
	static const EGLint context_attributes[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE,
	};
	importer->context = eglCreateContext(
		importer->display, config, EGL_NO_CONTEXT, context_attributes);
	if (importer->context == EGL_NO_CONTEXT) goto failed;
	static const EGLint surface_attributes[] = {
		EGL_WIDTH, 1,
		EGL_HEIGHT, 1,
		EGL_NONE,
	};
	importer->surface = eglCreatePbufferSurface(
		importer->display, config, surface_attributes);
	if (importer->surface == EGL_NO_SURFACE) {
		if (!extension_present(egl_extensions, "EGL_KHR_surfaceless_context"))
			goto failed;
	}
	if (!make_current(importer)) goto failed;
	const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
	if (!extension_present(gl_extensions, "GL_OES_EGL_image")) goto failed;
	importer->create_image = (PFNEGLCREATEIMAGEKHRPROC)
		eglGetProcAddress("eglCreateImageKHR");
	importer->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)
		eglGetProcAddress("eglDestroyImageKHR");
	importer->query_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)
		eglGetProcAddress("eglQueryDmaBufModifiersEXT");
	importer->image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
		eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!importer->create_image || !importer->destroy_image ||
	    !importer->query_modifiers || !importer->image_target_texture ||
	    !create_program(importer) || !query_formats(importer))
		goto failed;
	glGenFramebuffers(1, &importer->framebuffer);
	glGenTextures(1, &importer->output_texture);
	if (!importer->framebuffer || !importer->output_texture) goto failed;
	fprintf(stderr,
	        "[wayland] EGL dma-buf importer %d.%d vendor=%s pairs=%zu\n",
	        major, minor, eglQueryString(importer->display, EGL_VENDOR),
	        importer->format_count);
	return importer;

failed:
	np_egl_importer_destroy(importer);
	return NULL;
}

void np_egl_importer_destroy(struct np_egl_importer *importer)
{
	if (!importer) return;
	if (importer->display != EGL_NO_DISPLAY &&
	    importer->context != EGL_NO_CONTEXT && make_current(importer)) {
		if (importer->output_texture)
			glDeleteTextures(1, &importer->output_texture);
		if (importer->framebuffer)
			glDeleteFramebuffers(1, &importer->framebuffer);
		if (importer->program) glDeleteProgram(importer->program);
	}
	if (importer->display != EGL_NO_DISPLAY) {
		eglMakeCurrent(importer->display, EGL_NO_SURFACE,
		               EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (importer->surface != EGL_NO_SURFACE)
			eglDestroySurface(importer->display, importer->surface);
		if (importer->context != EGL_NO_CONTEXT)
			eglDestroyContext(importer->display, importer->context);
		eglTerminate(importer->display);
	}
	if (importer->gbm) gbm_device_destroy(importer->gbm);
	if (importer->drm_fd >= 0) close(importer->drm_fd);
	free(importer->formats);
	free(importer);
}

const struct np_dmabuf_format_modifier *np_egl_importer_formats(
	const struct np_egl_importer *importer, size_t *count)
{
	if (count) *count = importer ? importer->format_count : 0;
	return importer ? importer->formats : NULL;
}

bool np_egl_importer_supports(
	const struct np_egl_importer *importer, uint32_t format, uint64_t modifier)
{
	if (!importer) return false;
	for (size_t index = 0; index < importer->format_count; index++) {
		if (importer->formats[index].format == format &&
		    importer->formats[index].modifier == modifier)
			return true;
	}
	return false;
}

struct np_egl_buffer *np_egl_buffer_import(
	struct np_egl_importer *importer, int fd,
	int32_t width, int32_t height, uint32_t format,
	uint32_t offset, uint32_t stride, uint64_t modifier)
{
	if (!importer || fd < 0 || width <= 0 || height <= 0 || stride == 0 ||
	    modifier == DRM_FORMAT_MOD_LINEAR ||
	    !np_egl_importer_supports(importer, format, modifier) ||
	    !make_current(importer))
		return NULL;
	EGLint attributes[] = {
		EGL_WIDTH, width,
		EGL_HEIGHT, height,
		EGL_LINUX_DRM_FOURCC_EXT, (EGLint)format,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(uint32_t)modifier,
		EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(uint32_t)(modifier >> 32),
		EGL_NONE,
	};
	EGLImageKHR image = importer->create_image(
		importer->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
		(EGLClientBuffer)NULL, attributes);
	if (image == EGL_NO_IMAGE_KHR) {
		fprintf(stderr,
		        "[wayland] EGL dma-buf import failed modifier=0x%016" PRIx64
		        " error=0x%x\n", modifier, eglGetError());
		return NULL;
	}
	struct np_egl_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		importer->destroy_image(importer->display, image);
		return NULL;
	}
	buffer->importer = importer;
	buffer->image = image;
	buffer->width = width;
	buffer->height = height;
	glGenTextures(1, &buffer->texture);
	glBindTexture(GL_TEXTURE_2D, buffer->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	while (glGetError() != GL_NO_ERROR) {}
	importer->image_target_texture(GL_TEXTURE_2D, image);
	GLenum error = glGetError();
	glBindTexture(GL_TEXTURE_2D, 0);
	if (!buffer->texture || error != GL_NO_ERROR) {
		fprintf(stderr, "[wayland] EGL image texture failed error=0x%x\n", error);
		np_egl_buffer_destroy(buffer);
		return NULL;
	}
	return buffer;
}

void np_egl_buffer_destroy(struct np_egl_buffer *buffer)
{
	if (!buffer) return;
	struct np_egl_importer *importer = buffer->importer;
	if (importer && make_current(importer)) {
		if (buffer->texture) glDeleteTextures(1, &buffer->texture);
		if (buffer->image != EGL_NO_IMAGE_KHR && importer->destroy_image)
			importer->destroy_image(importer->display, buffer->image);
	}
	free(buffer->pixels);
	free(buffer);
}

bool np_egl_buffer_read_bgra(
	struct np_egl_buffer *buffer, bool force_opaque,
	const unsigned char **pixels, int32_t *stride)
{
	if (pixels) *pixels = NULL;
	if (stride) *stride = 0;
	if (!buffer || !pixels || !stride || !make_current(buffer->importer))
		return false;
	if ((size_t)buffer->width > SIZE_MAX / 4 ||
	    (size_t)buffer->height >
	        SIZE_MAX / ((size_t)buffer->width * 4))
		return false;
	size_t size = (size_t)buffer->width * (size_t)buffer->height * 4;
	if (size != buffer->pixels_size) {
		void *resized = realloc(buffer->pixels, size);
		if (!resized) return false;
		buffer->pixels = resized;
		buffer->pixels_size = size;
	}
	struct np_egl_importer *importer = buffer->importer;
	glBindTexture(GL_TEXTURE_2D, importer->output_texture);
	if (importer->output_width != buffer->width ||
	    importer->output_height != buffer->height) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
		             buffer->width, buffer->height, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		importer->output_width = buffer->width;
		importer->output_height = buffer->height;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, importer->framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                     GL_TEXTURE_2D, importer->output_texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return false;
	static const GLfloat positions[] = {
		-1.0f, -1.0f,
		 1.0f, -1.0f,
		-1.0f,  1.0f,
		 1.0f,  1.0f,
	};
	static const GLfloat texcoords[] = {
		0.0f, 0.0f,
		1.0f, 0.0f,
		0.0f, 1.0f,
		1.0f, 1.0f,
	};
	glViewport(0, 0, buffer->width, buffer->height);
	glDisable(GL_BLEND);
	glUseProgram(importer->program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, buffer->texture);
	glUniform1i(importer->sampler_location, 0);
	glUniform1f(importer->force_opaque_location, force_opaque ? 1.0f : 0.0f);
	glEnableVertexAttribArray((GLuint)importer->position_location);
	glEnableVertexAttribArray((GLuint)importer->texcoord_location);
	glVertexAttribPointer((GLuint)importer->position_location,
	                      2, GL_FLOAT, GL_FALSE, 0, positions);
	glVertexAttribPointer((GLuint)importer->texcoord_location,
	                      2, GL_FLOAT, GL_FALSE, 0, texcoords);
	while (glGetError() != GL_NO_ERROR) {}
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glReadPixels(0, 0, buffer->width, buffer->height,
	             GL_RGBA, GL_UNSIGNED_BYTE, buffer->pixels);
	GLenum error = glGetError();
	glDisableVertexAttribArray((GLuint)importer->position_location);
	glDisableVertexAttribArray((GLuint)importer->texcoord_location);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (error != GL_NO_ERROR) {
		fprintf(stderr, "[wayland] dma-buf readback failed error=0x%x\n", error);
		return false;
	}
	*pixels = buffer->pixels;
	*stride = buffer->width * 4;
	return true;
}
