#include "np_venus.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>

#define VIRGL_RENDERER_UNSTABLE_APIS 1
#include <virglrenderer.h>

// The ANGLE SDK's patched libepoxy is linked statically. ANGLE remains a pair
// of bundled frameworks loaded on demand, so the Swift package does not expose
// EGL/GLES linkage.
typedef void *np_egl_display;
typedef void *np_egl_context;
typedef void *np_egl_config;
typedef unsigned int np_egl_boolean;
typedef unsigned int np_egl_enum;
typedef intptr_t np_egl_attrib;
typedef int np_egl_int;

#define NP_EGL_FALSE 0
#define NP_EGL_NONE 0x3038
#define NP_EGL_EXTENSIONS 0x3055
#define NP_EGL_VENDOR 0x3053
#define NP_EGL_VERSION 0x3054
#define NP_EGL_RED_SIZE 0x3024
#define NP_EGL_GREEN_SIZE 0x3023
#define NP_EGL_BLUE_SIZE 0x3022
#define NP_EGL_ALPHA_SIZE 0x3021
#define NP_EGL_SURFACE_TYPE 0x3033
#define NP_EGL_RENDERABLE_TYPE 0x3040
#define NP_EGL_PBUFFER_BIT 0x0001
#define NP_EGL_OPENGL_ES3_BIT 0x0040
#define NP_EGL_OPENGL_ES_API 0x30A0
#define NP_EGL_CONTEXT_CLIENT_VERSION 0x3098
#define NP_EGL_CONTEXT_MINOR_VERSION_KHR 0x30FB
#define NP_EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define NP_EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define NP_EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE 0x3209
#define NP_EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE 0x320A
#define NP_EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489

struct angle_egl {
	void *handle;
	np_egl_display display;
	np_egl_config config;
	np_egl_display (*get_platform_display)(np_egl_enum, void *, const np_egl_attrib *);
	void *(*get_proc_address)(const char *);
	np_egl_boolean (*initialize)(np_egl_display, np_egl_int *, np_egl_int *);
	np_egl_boolean (*bind_api)(np_egl_enum);
	np_egl_boolean (*choose_config)(np_egl_display, const np_egl_int *,
	                                np_egl_config *, np_egl_int, np_egl_int *);
	np_egl_context (*create_context)(np_egl_display, np_egl_config,
	                                 np_egl_context, const np_egl_int *);
	np_egl_boolean (*destroy_context)(np_egl_display, np_egl_context);
	np_egl_boolean (*make_current)(np_egl_display, void *, void *, np_egl_context);
	np_egl_context (*get_current_context)(void);
	np_egl_boolean (*terminate)(np_egl_display);
	np_egl_int (*get_error)(void);
	const char *(*query_string)(np_egl_display, np_egl_int);
};

struct virgl_box {
	uint32_t x, y, z;
	uint32_t w, h, d;
};

struct imported {
	np_venus_blob blob;
	bool virgl_3d;
	bool mapped_by_renderer;
	void *mtl_texture;
	enum virgl_renderer_native_handle_type mtl_handle_type;
	uint32_t mtl_width;
	uint32_t mtl_height;
	uint32_t mtl_stride;
	uint32_t mtl_format;
	struct iovec *iov;
	uint32_t iov_count;
	struct imported *next;
};

struct renderer_context {
	uint32_t id;
	uint32_t capset_id;
	struct renderer_context *next;
};

static void executable_dir(char *out, size_t cap);

// libepoxy asks for these framework-relative names. Resolve them inside the
// VMHost bundle. Vulkan is not handled here: virglrenderer contains the static
// MoltenVK archive and calls vkGetInstanceProcAddr directly.
static void *np_dlopen(const char *path, int mode) {
	static void *(*next_dlopen)(const char *, int);
	if (!next_dlopen) {
		next_dlopen = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
	}
	if (path && (strcmp(path, "EGL.framework/Versions/Current/EGL") == 0 ||
	             strcmp(path, "GLESv2.framework/Versions/Current/GLESv2") == 0)) {
		char exe_dir[PATH_MAX];
		char framework[PATH_MAX];
		executable_dir(exe_dir, sizeof(exe_dir));
		int n = snprintf(framework, sizeof(framework), "%s/../Frameworks/%s",
		                 exe_dir, path);
		if (n > 0 && n < (int)sizeof(framework)) {
			void *handle = next_dlopen(framework, mode);
			if (handle) return handle;
		}
	}
	return next_dlopen(path, mode);
}

#define NP_DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void *replacement; const void *replacee; } \
	_interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = { \
		(const void *)(unsigned long)&_replacement, \
		(const void *)(unsigned long)&_replacee, \
	}
NP_DYLD_INTERPOSE(np_dlopen, dlopen);

// macOS posix shm often reports st_size 0 after ftruncate. virglrenderer's
// proxy then rejects the Venus ring fd. Recover the size from SEEK_END.
static int np_fstat(int fd, struct stat *st) {
	static int (*next_fstat)(int, struct stat *);
	if (!next_fstat) {
		next_fstat = (int (*)(int, struct stat *))dlsym(RTLD_NEXT, "fstat");
	}
	int rc = next_fstat(fd, st);
	if (rc == 0 && st && st->st_size == 0) {
		off_t cur = lseek(fd, 0, SEEK_CUR);
		off_t end = lseek(fd, 0, SEEK_END);
		if (cur >= 0) lseek(fd, cur, SEEK_SET);
		if (end > 0) st->st_size = end;
	}
	return rc;
}
NP_DYLD_INTERPOSE(np_fstat, fstat);

static void np_virgl_log(enum virgl_log_level_flags level,
                         const char *message, void *user) {
	(void)user;
	// virgl log levels are DEBUG=0, INFO=1, WARNING=2, ERROR=3.  Venus
	// reports linear-modifier emulation at INFO for each swapchain image and
	// often for every resize frame; forwarding that by default can make stderr
	// I/O more expensive than rendering.  Keep diagnostics, and expose the
	// verbose stream only when explicitly tracing the GPU.
	if (level < 2 && getenv("NATIVEPIPE_GPU_TRACE") == NULL &&
	    (!message || strstr(message, "vkr:") == NULL ||
	     strstr(message, "failed") == NULL)) return;
	fputs("[virgl] ", stderr);
	if (message && message[0]) {
		fputs(message, stderr);
		size_t n = strlen(message);
		if (message[n - 1] != '\n') fputc('\n', stderr);
	} else {
		fputc('\n', stderr);
	}
}

struct pending_fence {
	uint32_t ctx_id;
	uint32_t ring_idx;
	uint64_t fence_id;
	np_venus_fence_fn done;
	void *user;
	struct pending_fence *next;
};

struct np_venus {
	struct angle_egl egl;
	bool live;
	bool virgl_enabled;
	bool venus_enabled;
	uint32_t cap_version;
	uint32_t cap_size;

	pthread_mutex_t lock;
	pthread_mutex_t renderer_lock;
	struct imported *blobs;
	struct renderer_context *contexts;
	struct pending_fence *fences;
};

static void note(const char *fmt, ...) {
	if (getenv("NATIVEPIPE_GPU_TRACE") == NULL) return;
	va_list ap;
	va_start(ap, fmt);
	fputs("[venus] ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static bool is_virgl_capset(uint32_t capset_id) {
	return capset_id == 0 || capset_id == NP_VENUS_CAPSET_VIRGL ||
	       capset_id == NP_VENUS_CAPSET_VIRGL2;
}

/* virglrenderer caches its current vrend context globally, while EGL current
 * context is pthread-local. VZ dispatches device callbacks serially but may
 * move that serial queue between worker threads. Force vrend to rebind on the
 * entering thread, then release EGL ownership before returning to GCD. */
static void begin_renderer_call(np_venus *venus, bool bind_gl) {
	pthread_mutex_lock(&venus->renderer_lock);
	if (bind_gl && venus && venus->egl.display)
		virgl_renderer_force_ctx_0();
}

static void end_renderer_call(np_venus *venus, bool bind_gl) {
	if (bind_gl && venus && venus->egl.display && venus->egl.make_current &&
	    !venus->egl.make_current(venus->egl.display, NULL, NULL, NULL))
		note("ANGLE failed to release the current context: 0x%x",
		     venus->egl.get_error());
	pthread_mutex_unlock(&venus->renderer_lock);
}

static bool context_is_virgl(np_venus *venus, uint32_t ctx_id) {
	bool result = false;
	pthread_mutex_lock(&venus->lock);
	for (struct renderer_context *ctx = venus->contexts; ctx; ctx = ctx->next) {
		if (ctx->id == ctx_id) {
			result = is_virgl_capset(ctx->capset_id);
			break;
		}
	}
	pthread_mutex_unlock(&venus->lock);
	return result;
}

static bool context_exists(np_venus *venus, uint32_t ctx_id) {
	bool result = false;
	pthread_mutex_lock(&venus->lock);
	for (struct renderer_context *ctx = venus->contexts; ctx; ctx = ctx->next) {
		if (ctx->id == ctx_id) {
			result = true;
			break;
		}
	}
	pthread_mutex_unlock(&venus->lock);
	return result;
}

static bool resource_exists(np_venus *venus, uint32_t resource_id) {
	bool result = false;
	pthread_mutex_lock(&venus->lock);
	for (struct imported *res = venus->blobs; res; res = res->next) {
		if (res->blob.resource_id == resource_id) {
			result = true;
			break;
		}
	}
	pthread_mutex_unlock(&venus->lock);
	return result;
}

static void executable_dir(char *out, size_t cap) {
	uint32_t size = (uint32_t)cap;
	if (_NSGetExecutablePath(out, &size) != 0) {
		out[0] = '\0';
		return;
	}
	char *slash = strrchr(out, '/');
	if (slash) *slash = '\0';
}

static void *open_angle_egl(void) {
	const char *override = getenv("NATIVEPIPE_EGL");
	if (override && override[0]) {
		void *handle = dlopen(override, RTLD_NOW | RTLD_GLOBAL);
		if (handle) return handle;
		note("NATIVEPIPE_EGL=%s: %s", override, dlerror());
	}

	char exe[PATH_MAX];
	executable_dir(exe, sizeof(exe));
	if (exe[0]) {
		char path[PATH_MAX];
		const char *rel[] = {
			"/../Frameworks/EGL.framework/Versions/Current/EGL",
			"/../../../../vendor/angle-prefix/Frameworks/EGL.framework/Versions/Current/EGL",
			"/../../../vendor/angle-prefix/Frameworks/EGL.framework/Versions/Current/EGL",
			NULL,
		};
		for (const char **r = rel; *r; r++) {
			snprintf(path, sizeof(path), "%s%s", exe, *r);
			void *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
			if (handle) {
				note("loaded %s", path);
				return handle;
			}
		}
	}

	const char *candidates[] = {
		"vendor/angle-prefix/Frameworks/EGL.framework/Versions/Current/EGL",
		NULL,
	};
	for (const char **path = candidates; *path; path++) {
		void *handle = dlopen(*path, RTLD_NOW | RTLD_GLOBAL);
		if (handle) {
			note("loaded %s", *path);
			return handle;
		}
	}
	return NULL;
}

static void angle_egl_fini(struct angle_egl *egl) {
	if (!egl) return;
	if (egl->display && egl->terminate) egl->terminate(egl->display);
	if (egl->handle) dlclose(egl->handle);
	memset(egl, 0, sizeof(*egl));
}

static int angle_egl_init(struct angle_egl *egl) {
	memset(egl, 0, sizeof(*egl));
	egl->handle = open_angle_egl();
	if (!egl->handle) {
		note("bundled ANGLE EGL framework not found");
		return -1;
	}

	#define EGL_REQ(field, symbol) do { \
		egl->field = dlsym(egl->handle, symbol); \
		if (!egl->field) { note("missing ANGLE symbol %s", symbol); goto fail; } \
	} while (0)
	EGL_REQ(get_platform_display, "eglGetPlatformDisplay");
	EGL_REQ(get_proc_address, "eglGetProcAddress");
	EGL_REQ(initialize, "eglInitialize");
	EGL_REQ(bind_api, "eglBindAPI");
	EGL_REQ(choose_config, "eglChooseConfig");
	EGL_REQ(create_context, "eglCreateContext");
	EGL_REQ(destroy_context, "eglDestroyContext");
	EGL_REQ(make_current, "eglMakeCurrent");
	EGL_REQ(get_current_context, "eglGetCurrentContext");
	EGL_REQ(terminate, "eglTerminate");
	EGL_REQ(get_error, "eglGetError");
	EGL_REQ(query_string, "eglQueryString");
	#undef EGL_REQ

	const np_egl_attrib attrs[] = {
		NP_EGL_PLATFORM_ANGLE_TYPE_ANGLE,
		NP_EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
		NP_EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,
		NP_EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
		NP_EGL_NONE,
	};
	const np_egl_int ext_attrs[] = {
		NP_EGL_PLATFORM_ANGLE_TYPE_ANGLE,
		NP_EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
		NP_EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,
		NP_EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
		NP_EGL_NONE,
	};
	note("ANGLE client extensions=%s",
	     egl->query_string(NULL, NP_EGL_EXTENSIONS));
	typedef np_egl_display (*get_platform_display_ext_fn)(
		np_egl_enum, void *, const np_egl_int *);
	get_platform_display_ext_fn get_platform_display_ext =
		(get_platform_display_ext_fn)egl->get_proc_address("eglGetPlatformDisplayEXT");
	note("ANGLE eglGetPlatformDisplayEXT=%p", get_platform_display_ext);
	if (get_platform_display_ext) {
		egl->display = get_platform_display_ext(
			NP_EGL_PLATFORM_ANGLE_ANGLE, NULL, ext_attrs);
	} else {
		egl->display = egl->get_platform_display(
			NP_EGL_PLATFORM_ANGLE_ANGLE, NULL, attrs);
	}
	if (!egl->display) {
		note("ANGLE Metal eglGetPlatformDisplay failed: 0x%x", egl->get_error());
		goto fail;
	}

	np_egl_int major = 0, minor = 0;
	if (!egl->initialize(egl->display, &major, &minor)) {
		note("ANGLE eglInitialize failed: 0x%x", egl->get_error());
		goto fail;
	}
	if (!egl->bind_api(NP_EGL_OPENGL_ES_API)) {
		note("ANGLE eglBindAPI(OpenGL ES) failed: 0x%x", egl->get_error());
		goto fail;
	}
	const np_egl_int config_attrs[] = {
		NP_EGL_SURFACE_TYPE, NP_EGL_PBUFFER_BIT,
		NP_EGL_RENDERABLE_TYPE, NP_EGL_OPENGL_ES3_BIT,
		NP_EGL_RED_SIZE, 8,
		NP_EGL_GREEN_SIZE, 8,
		NP_EGL_BLUE_SIZE, 8,
		NP_EGL_ALPHA_SIZE, 8,
		NP_EGL_NONE,
	};
	np_egl_int config_count = 0;
	if (!egl->choose_config(egl->display, config_attrs, &egl->config, 1,
	                       &config_count) || config_count < 1) {
		note("ANGLE eglChooseConfig failed: 0x%x", egl->get_error());
		goto fail;
	}
	note("ANGLE EGL %d.%d vendor=%s version=%s", major, minor,
	     egl->query_string(egl->display, NP_EGL_VENDOR),
	     egl->query_string(egl->display, NP_EGL_VERSION));
	return 0;

fail:
	angle_egl_fini(egl);
	return -1;
}

static void finish_context_fence(np_venus *venus, uint32_t ctx_id, uint32_t ring_idx,
                                 uint64_t fence_id, bool success) {
	if (!venus) return;

	pthread_mutex_lock(&venus->lock);
	struct pending_fence **slot = &venus->fences;
	struct pending_fence *hit = NULL;
	while (*slot) {
		if ((*slot)->ctx_id == ctx_id && (*slot)->ring_idx == ring_idx &&
		    (*slot)->fence_id == fence_id) {
			hit = *slot;
			*slot = hit->next;
			break;
		}
		slot = &(*slot)->next;
	}
	pthread_mutex_unlock(&venus->lock);

	if (hit) {
		note("fence retired ctx=%u ring=%u id=%llu", ctx_id, ring_idx,
		     (unsigned long long)fence_id);
		hit->done(hit->user, hit->fence_id, success);
		free(hit);
	} else {
		note("fence retired ctx=%u ring=%u id=%llu (no waiter)", ctx_id, ring_idx,
		     (unsigned long long)fence_id);
	}
}

static void cancel_context_fences(np_venus *venus, uint32_t ctx_id) {
	struct pending_fence *cancelled = NULL;
	pthread_mutex_lock(&venus->lock);
	struct pending_fence **slot = &venus->fences;
	while (*slot) {
		if ((*slot)->ctx_id != ctx_id) {
			slot = &(*slot)->next;
			continue;
		}
		struct pending_fence *hit = *slot;
		*slot = hit->next;
		hit->next = cancelled;
		cancelled = hit;
	}
	pthread_mutex_unlock(&venus->lock);

	while (cancelled) {
		struct pending_fence *next = cancelled->next;
		cancelled->done(cancelled->user, cancelled->fence_id, false);
		free(cancelled);
		cancelled = next;
	}
}

static void write_context_fence(void *cookie, uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id) {
	finish_context_fence(cookie, ctx_id, ring_idx, fence_id, true);
}

static void write_fence(void *cookie, uint32_t fence) {
	write_context_fence(cookie, 0, 0, fence);
}

static virgl_renderer_gl_context create_gl_context(
	void *cookie, int scanout_idx, struct virgl_renderer_gl_ctx_param *param) {
	(void)scanout_idx;
	np_venus *venus = cookie;
	if (!venus || !venus->egl.display || !param) return NULL;
	const np_egl_int attrs[] = {
		NP_EGL_CONTEXT_CLIENT_VERSION, param->major_ver,
		NP_EGL_CONTEXT_MINOR_VERSION_KHR, param->minor_ver,
		NP_EGL_NONE,
	};
	np_egl_context share = param->shared
		? venus->egl.get_current_context()
		: NULL;
	np_egl_context context = venus->egl.create_context(
		venus->egl.display, venus->egl.config, share, attrs);
	if (!context) {
		note("ANGLE eglCreateContext ES %d.%d failed: 0x%x",
		     param->major_ver, param->minor_ver, venus->egl.get_error());
	}
	return context;
}

static void destroy_gl_context(void *cookie, virgl_renderer_gl_context context) {
	np_venus *venus = cookie;
	if (venus && venus->egl.display && context) {
		venus->egl.destroy_context(venus->egl.display, context);
	}
}

static int make_current(void *cookie, int scanout_idx,
	                    virgl_renderer_gl_context context) {
	(void)scanout_idx;
	np_venus *venus = cookie;
	if (!venus || !venus->egl.display) return -EINVAL;
	return venus->egl.make_current(
		venus->egl.display, NULL, NULL, context) ? 0 : -EIO;
}

static void *get_egl_display(void *cookie) {
	np_venus *venus = cookie;
	return venus ? venus->egl.display : NULL;
}

np_venus *np_venus_create(void) {
	np_venus *venus = calloc(1, sizeof(*venus));
	if (!venus) return NULL;
	pthread_mutex_init(&venus->lock, NULL);
	pthread_mutex_init(&venus->renderer_lock, NULL);

	if (angle_egl_init(&venus->egl) != 0) {
		note("ANGLE Metal is unavailable; fixed NativePipe GPU cannot start");
		return venus;
	}

	virgl_set_log_callback(np_virgl_log, NULL, NULL);
	// MoltenVK defaults to INFO, which prints its complete extension table for
	// every Venus process.  Warnings and errors remain visible; GPU trace mode
	// deliberately retains the upstream verbose default.
	if (!getenv("MVK_CONFIG_LOG_LEVEL") && !getenv("NATIVEPIPE_GPU_TRACE")) {
		setenv("MVK_CONFIG_LOG_LEVEL", "2", 0);
	}

	// virgl_renderer_init stores this pointer (state.cbs = cbs) and the
	// proxy sync thread dereferences it for every fence retire. A stack
	// local here means the first async fence jumps through freed stack.
	static struct virgl_renderer_callbacks cb;
	memset(&cb, 0, sizeof(cb));
	cb.version = VIRGL_RENDERER_CALLBACKS_VERSION;
	cb.write_fence = write_fence;
	cb.write_context_fence = write_context_fence;
	cb.create_gl_context = create_gl_context;
	cb.destroy_gl_context = destroy_gl_context;
	cb.make_current = make_current;
	cb.get_egl_display = get_egl_display;

	// UTM macos fork: omit VIRGL_RENDERER_RENDER_SERVER so proxy starts an
	// in-process render thread. Metal cannot share GPU resources across
	// processes; Venus + MoltenVK must live in this same address space.
	int flags = VIRGL_RENDERER_VENUS
	          | VIRGL_RENDERER_USE_GLES
	          | VIRGL_RENDERER_USE_SURFACELESS
	          | VIRGL_RENDERER_NATIVE_SHARE_TEXTURE
	          | VIRGL_RENDERER_THREAD_SYNC
	          | VIRGL_RENDERER_ASYNC_FENCE_CB;

	if (virgl_renderer_init(venus, flags, &cb) != 0) {
		note("virgl_renderer_init failed");
		angle_egl_fini(&venus->egl);
		return venus;
	}
	venus->virgl_enabled = true;

	virgl_renderer_get_cap_set(
		NP_VENUS_CAPSET_VENUS, &venus->cap_version, &venus->cap_size);
	uint32_t virgl_version = 0, virgl_size = 0;
	uint32_t virgl2_version = 0, virgl2_size = 0;
	virgl_renderer_get_cap_set(NP_VENUS_CAPSET_VIRGL, &virgl_version, &virgl_size);
	virgl_renderer_get_cap_set(NP_VENUS_CAPSET_VIRGL2, &virgl2_version, &virgl2_size);
	venus->venus_enabled = venus->cap_size > 0;
	venus->live = venus->venus_enabled && venus->virgl_enabled &&
	              virgl_size > 0 && virgl2_size > 0;
	note("virglrenderer up: virgl=%u/%u virgl2=%u/%u venus=%u/%u live=%d",
	     virgl_version, virgl_size, virgl2_version, virgl2_size,
	     venus->cap_version, venus->cap_size, venus->live);
	/* virgl_renderer_init leaves ctx0 current on its caller. Device callbacks
	 * may arrive on a different GCD worker, so do not retain thread ownership. */
	if (!venus->egl.make_current(venus->egl.display, NULL, NULL, NULL))
		note("ANGLE failed to release initial context: 0x%x", venus->egl.get_error());
	return venus;
}

void np_venus_destroy(np_venus *venus) {
	if (!venus) return;

	// Release native handles while their virgl resources still exist.
	while (true) {
		pthread_mutex_lock(&venus->lock);
		bool has_blob = venus->blobs != NULL;
		uint32_t resource_id = has_blob ? venus->blobs->blob.resource_id : 0;
		pthread_mutex_unlock(&venus->lock);
		if (!has_blob) break;
		np_venus_unimport_blob(venus, resource_id);
	}
	while (true) {
		pthread_mutex_lock(&venus->lock);
		bool has_context = venus->contexts != NULL;
		uint32_t context_id = has_context ? venus->contexts->id : 0;
		pthread_mutex_unlock(&venus->lock);
		if (!has_context) break;
		np_venus_context_destroy(venus, context_id);
	}

	pthread_mutex_lock(&venus->lock);
	struct pending_fence *fences = venus->fences;
	venus->fences = NULL;
	pthread_mutex_unlock(&venus->lock);
	while (fences) {
		struct pending_fence *next = fences->next;
		fences->done(fences->user, fences->fence_id, false);
		free(fences);
		fences = next;
	}

	if (venus->virgl_enabled) virgl_renderer_cleanup(venus);
	angle_egl_fini(&venus->egl);
	pthread_mutex_destroy(&venus->renderer_lock);
	pthread_mutex_destroy(&venus->lock);
	free(venus);
}

bool np_venus_is_live(const np_venus *venus) {
	return venus && venus->live;
}

void np_venus_capset_info(np_venus *venus, uint32_t *max_version, uint32_t *max_size) {
	np_renderer_capset_info(venus, NP_VENUS_CAPSET_VENUS, max_version, max_size);
}

void np_renderer_capset_info(np_venus *venus, uint32_t capset_id,
                             uint32_t *max_version, uint32_t *max_size) {
	if (!max_version || !max_size) return;
	if (!venus || !venus->live ||
	    (capset_id != NP_VENUS_CAPSET_VIRGL &&
	     capset_id != NP_VENUS_CAPSET_VIRGL2 &&
	     capset_id != NP_VENUS_CAPSET_VENUS)) {
		*max_version = 0;
		*max_size = 0;
		return;
	}
	if ((is_virgl_capset(capset_id) && !venus->virgl_enabled) ||
	    (capset_id == NP_VENUS_CAPSET_VENUS && !venus->venus_enabled)) {
		*max_version = 0;
		*max_size = 0;
		return;
	}
	begin_renderer_call(venus, is_virgl_capset(capset_id));
	virgl_renderer_get_cap_set(capset_id, max_version, max_size);
	end_renderer_call(venus, is_virgl_capset(capset_id));
}

uint32_t np_venus_fill_caps(np_venus *venus, uint32_t version, void *buffer, uint32_t buffer_size) {
	return np_renderer_fill_caps(
		venus, NP_VENUS_CAPSET_VENUS, version, buffer, buffer_size);
}

uint32_t np_renderer_fill_caps(np_venus *venus, uint32_t capset_id,
                               uint32_t version, void *buffer, uint32_t buffer_size) {
	uint32_t max_version = 0, max_size = 0;
	np_renderer_capset_info(venus, capset_id, &max_version, &max_size);
	(void)max_version;
	if (!buffer || !max_size || buffer_size < max_size) return 0;
	memset(buffer, 0, buffer_size);
	bool gl = is_virgl_capset(capset_id);
	begin_renderer_call(venus, gl);
	virgl_renderer_fill_caps(capset_id, version, buffer);
	end_renderer_call(venus, gl);
	return max_size;
}

int np_venus_context_create(np_venus *venus, uint32_t ctx_id, uint32_t capset_id,
                            const char *name) {
	if (!venus || !venus->live) return 0;
	if (capset_id != 0 && capset_id != NP_VENUS_CAPSET_VIRGL &&
	    capset_id != NP_VENUS_CAPSET_VIRGL2 &&
	    capset_id != NP_VENUS_CAPSET_VENUS) return -1;
	if ((is_virgl_capset(capset_id) && !venus->virgl_enabled) ||
	    (capset_id == NP_VENUS_CAPSET_VENUS && !venus->venus_enabled)) return -1;
	if (!ctx_id || context_exists(venus, ctx_id)) return -EEXIST;
	const char *label = name ? name : "";
	bool gl = is_virgl_capset(capset_id);
	begin_renderer_call(venus, gl);
	int rc = virgl_renderer_context_create_with_flags(
		ctx_id, capset_id & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK,
		(uint32_t)strlen(label), label);
	end_renderer_call(venus, gl);
	if (rc != 0) return rc;

	struct renderer_context *record = calloc(1, sizeof(*record));
	if (!record) {
		begin_renderer_call(venus, gl);
		virgl_renderer_context_destroy(ctx_id);
		end_renderer_call(venus, gl);
		return -ENOMEM;
	}
	record->id = ctx_id;
	record->capset_id = capset_id;
	pthread_mutex_lock(&venus->lock);
	record->next = venus->contexts;
	venus->contexts = record;
	pthread_mutex_unlock(&venus->lock);
	return 0;
}

int np_renderer_resource_create_3d(np_venus *venus,
                                   const np_renderer_resource_3d *resource) {
	if (!venus || !venus->live || !resource || !resource->resource_id) return -EINVAL;
	if (resource_exists(venus, resource->resource_id)) return -EEXIST;
	struct virgl_renderer_resource_create_args args = {
		.handle = resource->resource_id,
		.target = resource->target,
		.format = resource->format,
		.bind = resource->bind,
		.width = resource->width,
		.height = resource->height,
		.depth = resource->depth,
		.array_size = resource->array_size,
		.last_level = resource->last_level,
		.nr_samples = resource->nr_samples,
		.flags = resource->flags,
	};
	begin_renderer_call(venus, true);
	int rc = virgl_renderer_resource_create(&args, NULL, 0);
	end_renderer_call(venus, true);
	if (rc != 0) return rc;

	struct imported *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		begin_renderer_call(venus, true);
		virgl_renderer_resource_unref(resource->resource_id);
		end_renderer_call(venus, true);
		return -ENOMEM;
	}
	entry->blob.resource_id = resource->resource_id;
	entry->virgl_3d = true;
	pthread_mutex_lock(&venus->lock);
	entry->next = venus->blobs;
	venus->blobs = entry;
	pthread_mutex_unlock(&venus->lock);
	return 0;
}

int np_renderer_resource_attach_iov(np_venus *venus, uint32_t resource_id,
                                    const np_renderer_iovec *entries, uint32_t count) {
	if (!venus || !venus->live || !entries || !count || count > INT_MAX) return -EINVAL;
	if (!resource_exists(venus, resource_id)) return -ENOENT;
	struct iovec *iov = calloc(count, sizeof(*iov));
	if (!iov) return -ENOMEM;
	for (uint32_t i = 0; i < count; i++) {
		iov[i].iov_base = entries[i].base;
		iov[i].iov_len = entries[i].length;
	}
	begin_renderer_call(venus, true);
	int rc = virgl_renderer_resource_attach_iov((int)resource_id, iov, (int)count);
	end_renderer_call(venus, true);
	if (rc != 0) {
		free(iov);
		return rc;
	}
	pthread_mutex_lock(&venus->lock);
	for (struct imported *cur = venus->blobs; cur; cur = cur->next) {
		if (cur->blob.resource_id == resource_id) {
			cur->iov = iov;
			cur->iov_count = count;
			pthread_mutex_unlock(&venus->lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&venus->lock);
	begin_renderer_call(venus, true);
	virgl_renderer_resource_detach_iov((int)resource_id, NULL, NULL);
	end_renderer_call(venus, true);
	free(iov);
	return -ENOENT;
}

void np_renderer_resource_detach_iov(np_venus *venus, uint32_t resource_id) {
	if (!venus || !venus->live) return;
	if (!resource_exists(venus, resource_id)) return;
	struct iovec *iov = NULL;
	pthread_mutex_lock(&venus->lock);
	for (struct imported *cur = venus->blobs; cur; cur = cur->next) {
		if (cur->blob.resource_id == resource_id) {
			iov = cur->iov;
			cur->iov = NULL;
			cur->iov_count = 0;
			break;
		}
	}
	pthread_mutex_unlock(&venus->lock);
	begin_renderer_call(venus, true);
	virgl_renderer_resource_detach_iov((int)resource_id, NULL, NULL);
	end_renderer_call(venus, true);
	free(iov);
}

int np_renderer_transfer_3d(np_venus *venus, uint32_t resource_id, uint32_t ctx_id,
                            uint32_t level, uint32_t stride, uint32_t layer_stride,
                            const np_renderer_box *box, uint64_t offset,
                            bool from_host) {
	if (!venus || !venus->live || !box) return -EINVAL;
	if (!resource_exists(venus, resource_id) ||
	    (ctx_id && !context_exists(venus, ctx_id))) return -ENOENT;
	struct virgl_box wire_box = {
		.x = box->x, .y = box->y, .z = box->z,
		.w = box->width, .h = box->height, .d = box->depth,
	};
	begin_renderer_call(venus, true);
	int rc;
	if (from_host) {
		rc = virgl_renderer_transfer_read_iov(
			resource_id, ctx_id, level, stride, layer_stride,
			&wire_box, offset, NULL, 0);
	} else {
		rc = virgl_renderer_transfer_write_iov(
			resource_id, ctx_id, (int)level, stride, layer_stride,
			&wire_box, offset, NULL, 0);
	}
	end_renderer_call(venus, true);
	return rc;
}

void np_venus_context_destroy(np_venus *venus, uint32_t ctx_id) {
	if (!venus || !venus->live) return;
	bool gl = false;
	bool found = false;
	pthread_mutex_lock(&venus->lock);
	struct renderer_context **slot = &venus->contexts;
	while (*slot) {
		if ((*slot)->id == ctx_id) {
			struct renderer_context *record = *slot;
			*slot = record->next;
			gl = is_virgl_capset(record->capset_id);
			found = true;
			free(record);
			break;
		}
		slot = &(*slot)->next;
	}
	pthread_mutex_unlock(&venus->lock);
	if (!found) return;
	cancel_context_fences(venus, ctx_id);
	begin_renderer_call(venus, gl);
	virgl_renderer_context_destroy(ctx_id);
	end_renderer_call(venus, gl);
}

int np_venus_create_blob(np_venus *venus, uint32_t ctx_id, np_venus_blob *blob) {
	if (!venus || !blob || blob->size == 0) return -EINVAL;
	if (!blob->resource_id || resource_exists(venus, blob->resource_id)) return -EEXIST;

	struct imported *entry = calloc(1, sizeof(*entry));
	if (!entry) return -ENOMEM;
	entry->blob = *blob;
	entry->virgl_3d = venus->live && context_is_virgl(venus, ctx_id);

	note("blob res=%u ctx=%u blob_id=%llu %ux%u %llu bytes",
	     blob->resource_id, ctx_id, (unsigned long long)blob->blob_id,
		     0u, 0u, (unsigned long long)blob->size);

	if (!venus->live) {
		pthread_mutex_lock(&venus->lock);
		entry->next = venus->blobs;
		venus->blobs = entry;
		pthread_mutex_unlock(&venus->lock);
		return 0;
	}
	if (!context_exists(venus, ctx_id)) {
		free(entry);
		return -ENOENT;
	}

	struct virgl_renderer_resource_create_blob_args args;
	memset(&args, 0, sizeof(args));
	args.res_handle = blob->resource_id;
	args.ctx_id = ctx_id;
	args.blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D;
	/* Older virtio-gpu guests leave the Venus ring flags empty even though
	 * they immediately MAP_BLOB it.  Keep that compatibility narrowly on the
	 * blob_id == 0 ring path; real VkDeviceMemory exports retain their exact
	 * flags. */
	args.blob_flags = blob->blob_flags;
	if (blob->blob_id == 0 && args.blob_flags == 0)
		args.blob_flags = VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE;
	args.blob_id = blob->blob_id;
	args.size = blob->size;
	begin_renderer_call(venus, entry->virgl_3d);
	int rc = virgl_renderer_resource_create_blob(&args);
	if (rc != 0) {
		end_renderer_call(venus, entry->virgl_3d);
		// Compositor packed-geometry ids are not vkr objects. Mesa
		// blob_id=0 shm and real Venus memory must not be swallowed:
		// a fake OK makes the guest reserve a hole the host cannot map.
		note("resource_create_blob res=%u blob_id=%llu flags=0x%x rc=%d%s",
		     blob->resource_id, (unsigned long long)blob->blob_id,
		     args.blob_flags, rc,
		     "");
		free(entry);
		return rc;
	}

	// Mesa Venus: the pages already exist inside vkr. Hand the mapping
	// back so virtio-gpu maps this pointer, not a second IOSurface.
	// resource_map is meaningful only when the guest requested a mappable blob.
	// Non-mappable resources must remain on the renderer's native path.
	if (args.blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE) {
		void *map = NULL;
		uint64_t map_size = 0;
		if (virgl_renderer_resource_map(blob->resource_id, &map, &map_size) == 0 && map) {
			blob->pointer = map;
			if (map_size > 0) blob->size = map_size;
			entry->blob.pointer = map;
			entry->blob.size = blob->size;
			entry->mapped_by_renderer = true;
			note("blob res=%u mapped by vkr at %p size=%llu",
			     blob->resource_id, map, (unsigned long long)blob->size);
		} else {
			note("blob res=%u renderer map unavailable (flags=0x%x)",
			     blob->resource_id, args.blob_flags);
			virgl_renderer_resource_unref(blob->resource_id);
			end_renderer_call(venus, entry->virgl_3d);
			free(entry);
			return -EINVAL;
		}
	}
	end_renderer_call(venus, entry->virgl_3d);
	pthread_mutex_lock(&venus->lock);
	entry->next = venus->blobs;
	venus->blobs = entry;
	pthread_mutex_unlock(&venus->lock);
	return 0;
}

void np_venus_unimport_blob(np_venus *venus, uint32_t resource_id) {
	if (!venus) return;

	pthread_mutex_lock(&venus->lock);
	struct imported **slot = &venus->blobs;
	while (*slot) {
		if ((*slot)->blob.resource_id == resource_id) {
			struct imported *hit = *slot;
			*slot = hit->next;
			pthread_mutex_unlock(&venus->lock);
			begin_renderer_call(venus, hit->virgl_3d);
			if (hit->iov && venus->live) {
				virgl_renderer_resource_detach_iov((int)resource_id, NULL, NULL);
				free(hit->iov);
			}
			if (hit->mapped_by_renderer) {
				virgl_renderer_resource_unmap(resource_id);
			}
			if (hit->mtl_texture) {
				if (hit->mtl_handle_type != VIRGL_NATIVE_HANDLE_NONE) {
					virgl_renderer_release_handle_for_scanout(
						hit->mtl_handle_type, hit->mtl_texture);
				} else {
					CFRelease(hit->mtl_texture);
				}
			}
			bool gl = hit->virgl_3d;
			free(hit);
			if (venus->live) virgl_renderer_resource_unref(resource_id);
			end_renderer_call(venus, gl);
			return;
		}
		slot = &(*slot)->next;
	}
	pthread_mutex_unlock(&venus->lock);
}

void *np_venus_metal_texture(np_venus *venus, uint32_t resource_id,
                             uint32_t width, uint32_t height,
                             uint32_t stride, uint32_t virgl_format) {
	if (!venus || !venus->live) return NULL;
	bool gl = false;
	bool found = false;
	void *old_texture = NULL;
	enum virgl_renderer_native_handle_type old_type = VIRGL_NATIVE_HANDLE_NONE;

	pthread_mutex_lock(&venus->lock);
	for (struct imported *cur = venus->blobs; cur; cur = cur->next) {
		if (cur->blob.resource_id != resource_id) continue;
		found = true;
		if (cur->mtl_texture && width <= cur->mtl_width &&
		    height <= cur->mtl_height && stride == cur->mtl_stride &&
		    virgl_format == cur->mtl_format) {
			void *tex = cur->mtl_texture;
			pthread_mutex_unlock(&venus->lock);
			return tex;
		}
		gl = cur->virgl_3d;
		old_texture = cur->mtl_texture;
		old_type = cur->mtl_handle_type;
		cur->mtl_texture = NULL;
		cur->mtl_handle_type = VIRGL_NATIVE_HANDLE_NONE;
		break;
	}
	pthread_mutex_unlock(&venus->lock);
	if (!found) return NULL;

	/* Never hold venus->lock while entering virglrenderer. Fence callbacks run
	 * on renderer threads and acquire venus->lock; taking the locks in the
	 * opposite order here deadlocks a submit against texture export. */
	begin_renderer_call(venus, gl);
	if (old_texture) {
		if (old_type != VIRGL_NATIVE_HANDLE_NONE)
			virgl_renderer_release_handle_for_scanout(old_type, old_texture);
		else
			CFRelease(old_texture);
	}
	void *handle = NULL;
	enum virgl_renderer_native_handle_type type = VIRGL_NATIVE_HANDLE_NONE;
	if (width && height) {
		type = virgl_renderer_create_handle_for_scanout(
			resource_id, width, height, virgl_format, 0, stride, 0, &handle);
	}
	end_renderer_call(venus, gl);

	bool adopted = false;
	if (type == VIRGL_NATIVE_HANDLE_METAL_TEXTURE && handle) {
		pthread_mutex_lock(&venus->lock);
		for (struct imported *cur = venus->blobs; cur; cur = cur->next) {
			if (cur->blob.resource_id != resource_id) continue;
			cur->mtl_texture = handle;
			cur->mtl_handle_type = type;
			cur->mtl_width = width;
			cur->mtl_height = height;
			cur->mtl_stride = stride;
			cur->mtl_format = virgl_format;
			adopted = true;
			break;
		}
		pthread_mutex_unlock(&venus->lock);
	}
	if (adopted) {
		note("blob res=%u scanout metal texture %p %ux%u stride=%u fmt=%u",
		     resource_id, handle, width, height, stride, virgl_format);
		return handle;
	}
	if (handle) {
		begin_renderer_call(venus, gl);
		if (type != VIRGL_NATIVE_HANDLE_NONE)
			virgl_renderer_release_handle_for_scanout(type, handle);
		else
			CFRelease(handle);
		end_renderer_call(venus, gl);
	}
	note("blob res=%u create_handle_for_scanout type=%d handle=%p",
	     resource_id, type, handle);
	return NULL;
}

int np_venus_attach(np_venus *venus, uint32_t ctx_id, uint32_t resource_id) {
	if (!venus || !venus->live) return 0;
	if (!context_exists(venus, ctx_id) || !resource_exists(venus, resource_id))
		return -ENOENT;
	bool gl = context_is_virgl(venus, ctx_id);
	begin_renderer_call(venus, gl);
	virgl_renderer_ctx_attach_resource((int)ctx_id, (int)resource_id);
	end_renderer_call(venus, gl);
	return 0;
}

int np_venus_detach(np_venus *venus, uint32_t ctx_id, uint32_t resource_id) {
	if (!venus || !venus->live) return 0;
	if (!context_exists(venus, ctx_id) || !resource_exists(venus, resource_id))
		return -ENOENT;
	bool gl = context_is_virgl(venus, ctx_id);
	begin_renderer_call(venus, gl);
	virgl_renderer_ctx_detach_resource((int)ctx_id, (int)resource_id);
	end_renderer_call(venus, gl);
	return 0;
}

int np_venus_submit(np_venus *venus, uint32_t ctx_id, uint32_t ring_idx,
                    const void *payload, uint32_t byte_count,
                    bool wants_fence, uint64_t fence_id,
                    np_venus_fence_fn done, void *user) {
	if (!done) return -1;

	if (!venus || !venus->live) {
		// Mesa only submits after GET_CAPSET advertised Venus. If we got
		// here without a renderer, saying OK would lie about the GPU.
		done(user, fence_id, false);
		return -1;
	}

	if (byte_count % 4 != 0) {
		done(user, fence_id, false);
		return -1;
	}
	if (!context_exists(venus, ctx_id)) {
		done(user, fence_id, false);
		return -ENOENT;
	}

	if (wants_fence) {
		struct pending_fence *pending = calloc(1, sizeof(*pending));
		if (!pending) {
			done(user, fence_id, false);
			return -1;
		}
		pending->fence_id = fence_id;
		pending->ctx_id = ctx_id;
		pending->ring_idx = ring_idx;
		pending->done = done;
		pending->user = user;
		pthread_mutex_lock(&venus->lock);
		pending->next = venus->fences;
		venus->fences = pending;
		pthread_mutex_unlock(&venus->lock);
	}

	bool gl = context_is_virgl(venus, ctx_id);
	begin_renderer_call(venus, gl);
	int rc = virgl_renderer_submit_cmd(
		(void *)payload, (int)ctx_id, (int)(byte_count / 4));
	if (rc != 0) {
		end_renderer_call(venus, gl);
		if (wants_fence)
			finish_context_fence(venus, ctx_id, ring_idx, fence_id, false);
		else done(user, fence_id, false);
		return rc;
	}

	bool finish_now = false;
	if (wants_fence) {
		// The ring index picks the timeline: 0 retires on decode, a queue
		// ring retires when MoltenVK finished the work. Passing the guest's
		// ring through is what makes the fence truthful.
		int frc = virgl_renderer_context_create_fence(
			ctx_id, VIRGL_RENDERER_FENCE_FLAG_MERGEABLE, ring_idx, fence_id);
		if (frc != 0) {
			note("context_create_fence ring=%u id=%llu failed rc=%d; retiring now",
			     ring_idx, (unsigned long long)fence_id, frc);
			finish_now = true;
		}
	} else {
		finish_now = true;
	}
	end_renderer_call(venus, gl);
	if (finish_now) {
		if (wants_fence)
			finish_context_fence(venus, ctx_id, ring_idx, fence_id, false);
		else
			done(user, fence_id, true);
	}
	return 0;
}
