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
#include <time.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>

// virglrenderer symbols, resolved at runtime. Missing symbols just mean Venus
// is not live — the rest of NativePipe (wl_shm windows, blob mapping) keeps
// working.
#define VIRGL_RENDERER_THREAD_SYNC        (1 << 1)
#define VIRGL_RENDERER_USE_SURFACELESS    (1 << 3)
#define VIRGL_RENDERER_VENUS              (1 << 6)
#define VIRGL_RENDERER_NO_VIRGL           (1 << 7)
#define VIRGL_RENDERER_ASYNC_FENCE_CB     (1 << 8)
#define VIRGL_RENDERER_RENDER_SERVER      (1 << 9)

#define VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK 0xff
#define VIRGL_RENDERER_BLOB_MEM_HOST3D    0x0002
#define VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE 0x0001

#define VIRGL_RENDERER_FENCE_FLAG_MERGEABLE (1 << 0)

struct virgl_renderer_gl_ctx_param;
typedef void *virgl_renderer_gl_context;

struct virgl_renderer_callbacks {
	int version;
	void (*write_fence)(void *cookie, uint32_t fence);
	virgl_renderer_gl_context (*create_gl_context)(void *cookie, int, struct virgl_renderer_gl_ctx_param *);
	void (*destroy_gl_context)(void *cookie, virgl_renderer_gl_context);
	int (*make_current)(void *cookie, int, virgl_renderer_gl_context);
	int (*get_drm_fd)(void *cookie);
	void (*write_context_fence)(void *cookie, uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id);
	/* v3 */
	int (*get_server_fd)(void *cookie, uint32_t version);
	/* v4 */
	void *(*get_egl_display)(void *cookie);
};

struct virgl_renderer_resource_create_blob_args {
	uint32_t res_handle;
	uint32_t ctx_id;
	uint32_t blob_mem;
	uint32_t blob_flags;
	uint64_t blob_id;
	uint64_t size;
	const void *iovecs;
	uint32_t num_iovs;
};

struct virgl_syms {
	void *handle;
	int (*init)(void *cookie, int flags, struct virgl_renderer_callbacks *cb);
	void (*cleanup)(void *cookie);
	void (*reset)(void);
	void (*poll)(void);
	void (*get_cap_set)(uint32_t set, uint32_t *max_ver, uint32_t *max_size);
	void (*fill_caps)(uint32_t set, uint32_t version, void *caps);
	int (*context_create_with_flags)(uint32_t ctx_id, uint32_t ctx_flags,
	                                 uint32_t nlen, const char *name);
	void (*context_destroy)(uint32_t handle);
	int (*submit_cmd)(void *buffer, int ctx_id, int ndw);
	int (*resource_create_blob)(const struct virgl_renderer_resource_create_blob_args *args);
	void (*resource_unref)(uint32_t res_handle);
	int (*resource_map)(uint32_t res_handle, void **map, uint64_t *out_size);
	int (*resource_unmap)(uint32_t res_handle);
	int (*create_handle_for_scanout)(uint32_t res_id, uint32_t width, uint32_t height,
	                                 uint32_t virgl_format, uint32_t padding,
	                                 uint32_t stride, uint32_t offset, void **handle);
	void (*release_handle_for_scanout)(int type, void *handle);
	void (*ctx_attach_resource)(int ctx_id, int res_handle);
	void (*ctx_detach_resource)(int ctx_id, int res_handle);
	int (*context_create_fence)(uint32_t ctx_id, uint32_t flags,
	                            uint32_t ring_idx, uint64_t fence_id);
	void (*set_log_callback)(void (*cb)(int, const char *, void *),
	                         void *user, void (*free_user)(void *));
};

struct imported {
	np_venus_blob blob;
	bool mapped_by_renderer;
	void *mtl_texture;
	int mtl_handle_type;
	uint32_t mtl_width;
	uint32_t mtl_height;
	uint32_t mtl_stride;
	uint32_t mtl_format;
	struct imported *next;
};

static void executable_dir(char *out, size_t cap);

// vkr dlopens "libvulkan.dylib" then "libMoltenVK.dylib" by leaf name.
// Those names are not on the default search path. Redirect both to the
// MoltenVK dylib so the host ICD loads without a Vulkan loader. Diagnostics
// may explicitly name a Vulkan loader so VKR_DEBUG=validate can insert VVL;
// the production/App Store path remains the bundled, loader-free MoltenVK.
static void *np_dlopen(const char *path, int mode) {
	static void *(*next_dlopen)(const char *, int);
	char bundled_mvk[PATH_MAX] = {0};
	if (!next_dlopen) {
		next_dlopen = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
	}
	if (path && (strcmp(path, "libvulkan.dylib") == 0 ||
	             strcmp(path, "libvulkan.1.dylib") == 0)) {
		const char *loader = getenv("NATIVEPIPE_VULKAN_LOADER");
		if (loader && loader[0]) {
			dlerror();
			void *handle = next_dlopen(loader, mode);
			if (handle) {
				fprintf(stderr, "[venus] validation Vulkan loader %s\n", loader);
				return handle;
			}
			fprintf(stderr, "[venus] validation Vulkan loader %s failed: %s\n",
			        loader, dlerror());
		}
	}
	if (path && (strcmp(path, "libvulkan.dylib") == 0 ||
	             strcmp(path, "libvulkan.1.dylib") == 0 ||
	             strcmp(path, "libMoltenVK.dylib") == 0)) {
		const char *mvk = getenv("NATIVEPIPE_MOLTENVK");
		if (!mvk || !mvk[0]) {
			char exe_dir[PATH_MAX];
			executable_dir(exe_dir, sizeof(exe_dir));
			int bundled_len = exe_dir[0]
			    ? snprintf(bundled_mvk, sizeof(bundled_mvk),
			               "%s/../Frameworks/libMoltenVK.dylib", exe_dir)
			    : -1;
			if (bundled_len > 0 && bundled_len < (int)sizeof(bundled_mvk) &&
			    access(bundled_mvk, R_OK) == 0) {
				mvk = bundled_mvk;
			}
			static const char *const candidates[] = {
				"vendor/moltenvk-prefix/lib/libMoltenVK.dylib",
				"/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib",
				NULL,
			};
			if (!mvk || !mvk[0]) {
				for (int i = 0; candidates[i]; i++) {
					if (access(candidates[i], R_OK) == 0) {
						mvk = candidates[i];
						break;
					}
				}
			}
		}
		void *handle = mvk && mvk[0] ? next_dlopen(mvk, mode) : NULL;
		if (handle) return handle;
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

static void np_virgl_log(int level, const char *message, void *user) {
	(void)user;
	// virgl log levels are DEBUG=0, INFO=1, WARNING=2, ERROR=3.  Venus
	// reports linear-modifier emulation at INFO for each swapchain image and
	// often for every resize frame; forwarding that by default can make stderr
	// I/O more expensive than rendering.  Keep diagnostics, and expose the
	// verbose stream only when explicitly tracing the GPU.
	if (level < 2 && getenv("NATIVEPIPE_GPU_TRACE") == NULL) return;
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
	struct virgl_syms virgl;
	bool live;
	uint32_t cap_version;
	uint32_t cap_size;

	pthread_mutex_t lock;
	struct imported *blobs;
	struct pending_fence *fences;
	bool owns_global_renderer;
};

// virglrenderer and its in-process vkr server are process-global.  Full
// cleanup + re-init leaves the UTM macOS renderer able to report a capset but
// unable to enumerate MoltenVK devices on its second lifetime.  Keep one live
// renderer for the host process and hand out exclusive VM leases; reset
// contexts/resources between leases instead of unloading the render server.
static np_venus *g_venus;
static np_venus *g_cached_renderer;
static pthread_mutex_t g_venus_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_venus_changed = PTHREAD_COND_INITIALIZER;

static np_venus *acquire_global_renderer(np_venus *candidate) {
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;

	pthread_mutex_lock(&g_venus_lock);
	while (g_venus) {
		int rc = pthread_cond_timedwait(&g_venus_changed, &g_venus_lock, &deadline);
		if (rc == ETIMEDOUT) {
			pthread_mutex_unlock(&g_venus_lock);
			return NULL;
		}
	}
	np_venus *venus = g_cached_renderer ? g_cached_renderer : candidate;
	g_venus = venus;
	venus->owns_global_renderer = true;
	pthread_mutex_unlock(&g_venus_lock);
	return venus;
}

static void cache_global_renderer(np_venus *venus) {
	pthread_mutex_lock(&g_venus_lock);
	if (!g_cached_renderer) g_cached_renderer = venus;
	pthread_mutex_unlock(&g_venus_lock);
}

static void release_global_renderer(np_venus *venus) {
	pthread_mutex_lock(&g_venus_lock);
	if (g_venus == venus) {
		g_venus = NULL;
		pthread_cond_broadcast(&g_venus_changed);
	}
	venus->owns_global_renderer = false;
	pthread_mutex_unlock(&g_venus_lock);
}

static void note(const char *fmt, ...) {
	if (getenv("NATIVEPIPE_GPU_TRACE") == NULL) return;
	va_list ap;
	va_start(ap, fmt);
	fputs("[venus] ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void *load_sym(void *handle, const char *name) {
	void *sym = dlsym(handle, name);
	if (!sym) note("missing symbol %s", name);
	return sym;
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

static void *try_dlopen(const char *path) {
	if (!path || !path[0]) return NULL;
	void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (handle) note("loaded %s", path);
	return handle;
}

static void *open_virglrenderer(void) {
	const char *override = getenv("NATIVEPIPE_VIRGLRENDERER");
	if (override && override[0]) {
		void *handle = try_dlopen(override);
		if (handle) return handle;
		note("NATIVEPIPE_VIRGLRENDERER=%s: %s", override, dlerror());
	}

	char exe[PATH_MAX];
	executable_dir(exe, sizeof(exe));
	if (exe[0]) {
		char path[PATH_MAX];
		const char *rel[] = {
			"/../Frameworks/libvirglrenderer.dylib",
			"/../Frameworks/libvirglrenderer.1.dylib",
			"/libvirglrenderer.dylib",
			NULL,
		};
		for (const char **r = rel; *r; r++) {
			snprintf(path, sizeof(path), "%s%s", exe, *r);
			void *handle = try_dlopen(path);
			if (handle) return handle;
		}
		// Dev tree: NativePipe.app/Contents/MacOS → walk up to vendor/.
		const char *vendor[] = {
			"/../../../../vendor/virglrenderer-prefix/lib/libvirglrenderer.dylib",
			"/../../../vendor/virglrenderer-prefix/lib/libvirglrenderer.dylib",
			NULL,
		};
		for (const char **r = vendor; *r; r++) {
			snprintf(path, sizeof(path), "%s%s", exe, *r);
			void *handle = try_dlopen(path);
			if (handle) return handle;
		}
	}

	const char *candidates[] = {
		"/usr/local/lib/libvirglrenderer.1.dylib",
		"/usr/local/lib/libvirglrenderer.dylib",
		"/opt/homebrew/lib/libvirglrenderer.1.dylib",
		"/opt/homebrew/lib/libvirglrenderer.dylib",
		NULL,
	};
	for (const char **path = candidates; *path; path++) {
		void *handle = try_dlopen(*path);
		if (handle) return handle;
	}
	return NULL;
}

static void finish_context_fence(void *cookie, uint32_t ctx_id, uint32_t ring_idx,
                                 uint64_t fence_id, bool success) {
	np_venus *venus = cookie;
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

static void write_context_fence(void *cookie, uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id) {
	finish_context_fence(cookie, ctx_id, ring_idx, fence_id, true);
}

static void write_fence(void *cookie, uint32_t fence) {
	write_context_fence(cookie, 0, 0, fence);
}

static int resolve_virgl(struct virgl_syms *virgl) {
	virgl->handle = open_virglrenderer();
	if (!virgl->handle) return -1;

	#define REQ(field, symbol) do { \
		virgl->field = load_sym(virgl->handle, symbol); \
		if (!virgl->field) return -1; \
	} while (0)

	REQ(init, "virgl_renderer_init");
	REQ(cleanup, "virgl_renderer_cleanup");
	REQ(reset, "virgl_renderer_reset");
	REQ(poll, "virgl_renderer_poll");
	REQ(get_cap_set, "virgl_renderer_get_cap_set");
	REQ(fill_caps, "virgl_renderer_fill_caps");
	REQ(context_create_with_flags, "virgl_renderer_context_create_with_flags");
	REQ(context_destroy, "virgl_renderer_context_destroy");
	REQ(submit_cmd, "virgl_renderer_submit_cmd");
	REQ(resource_unref, "virgl_renderer_resource_unref");
	REQ(ctx_attach_resource, "virgl_renderer_ctx_attach_resource");
	REQ(ctx_detach_resource, "virgl_renderer_ctx_detach_resource");
	REQ(resource_create_blob, "virgl_renderer_resource_create_blob");
	#undef REQ
	virgl->resource_map = load_sym(virgl->handle, "virgl_renderer_resource_map");
	virgl->resource_unmap = load_sym(virgl->handle, "virgl_renderer_resource_unmap");
	virgl->create_handle_for_scanout =
		load_sym(virgl->handle, "virgl_renderer_create_handle_for_scanout");
	virgl->release_handle_for_scanout =
		load_sym(virgl->handle, "virgl_renderer_release_handle_for_scanout");
	virgl->context_create_fence = load_sym(virgl->handle, "virgl_renderer_context_create_fence");
	virgl->set_log_callback = load_sym(virgl->handle, "virgl_set_log_callback");
	return 0;
}

np_venus *np_venus_create(void) {
	np_venus *candidate = calloc(1, sizeof(*candidate));
	if (!candidate) return NULL;
	pthread_mutex_init(&candidate->lock, NULL);
	np_venus *venus = acquire_global_renderer(candidate);
	if (!venus) {
		note("another VM still owns the process-global renderer; Venus disabled for this device");
		return candidate;
	}
	if (venus != candidate) {
		pthread_mutex_destroy(&candidate->lock);
		free(candidate);
		note("reusing process-global Venus renderer");
		return venus;
	}

	if (resolve_virgl(&venus->virgl) != 0) {
		note("virglrenderer not present; Mesa will not see a Venus capset");
		if (venus->virgl.handle) dlclose(venus->virgl.handle);
		memset(&venus->virgl, 0, sizeof(venus->virgl));
		release_global_renderer(venus);
		return venus;
	}

	if (venus->virgl.set_log_callback) {
		venus->virgl.set_log_callback(np_virgl_log, NULL, NULL);
	}
	// MoltenVK defaults to INFO, which prints its complete extension table for
	// every Venus process.  Warnings and errors remain visible; GPU trace mode
	// deliberately retains the upstream verbose default.
	if (!getenv("MVK_CONFIG_LOG_LEVEL") && !getenv("NATIVEPIPE_GPU_TRACE")) {
		setenv("MVK_CONFIG_LOG_LEVEL", "2", 0);
	}

	// MoltenVK as ICD. vkr with vulkan-dload dlopens libvulkan / libMoltenVK;
	// the interposer above sends both at this file. The ICD json is for a
	// loader, if one is ever present.
	if (!getenv("VK_ICD_FILENAMES")) {
		static const char *const icds[] = {
			"vendor/moltenvk-prefix/etc/vulkan/icd.d/MoltenVK_icd.json",
			"/opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json",
			NULL,
		};
		for (int i = 0; icds[i]; i++) {
			if (access(icds[i], R_OK) == 0) {
				setenv("VK_ICD_FILENAMES", icds[i], 0);
				break;
			}
		}
	}

	// virgl_renderer_init stores this pointer (state.cbs = cbs) and the
	// proxy sync thread dereferences it for every fence retire. A stack
	// local here means the first async fence jumps through freed stack.
	static struct virgl_renderer_callbacks cb;
	memset(&cb, 0, sizeof(cb));
	cb.version = 3;
	cb.write_fence = write_fence;
	cb.write_context_fence = write_context_fence;

	// UTM macos fork: omit VIRGL_RENDERER_RENDER_SERVER so proxy starts an
	// in-process render thread. Metal cannot share GPU resources across
	// processes; Venus + MoltenVK must live in this same address space.
	int flags = VIRGL_RENDERER_VENUS
	          | VIRGL_RENDERER_NO_VIRGL
	          | VIRGL_RENDERER_USE_SURFACELESS
	          | VIRGL_RENDERER_THREAD_SYNC
	          | VIRGL_RENDERER_ASYNC_FENCE_CB;

	if (venus->virgl.init(venus, flags, &cb) != 0) {
		note("virgl_renderer_init failed");
		dlclose(venus->virgl.handle);
		memset(&venus->virgl, 0, sizeof(venus->virgl));
		release_global_renderer(venus);
		return venus;
	}

	venus->virgl.get_cap_set(NP_VENUS_CAPSET_VENUS, &venus->cap_version, &venus->cap_size);
	venus->live = venus->cap_size > 0;
	if (venus->live) cache_global_renderer(venus);
	note("virglrenderer up: Venus capset v%u size=%u live=%d",
	     venus->cap_version, venus->cap_size, venus->live);
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

	pthread_mutex_lock(&g_venus_lock);
	bool persistent = g_cached_renderer == venus;
	pthread_mutex_unlock(&g_venus_lock);
	if (persistent) {
		if (venus->virgl.reset) venus->virgl.reset();
		if (venus->owns_global_renderer) release_global_renderer(venus);
		return;
	}

	if (venus->virgl.cleanup) venus->virgl.cleanup(venus);
	if (venus->virgl.handle) dlclose(venus->virgl.handle);
	if (venus->owns_global_renderer) release_global_renderer(venus);

	pthread_mutex_destroy(&venus->lock);
	free(venus);
}

bool np_venus_is_live(const np_venus *venus) {
	return venus && venus->live;
}

void np_venus_capset_info(np_venus *venus, uint32_t *max_version, uint32_t *max_size) {
	if (!venus || !venus->live) {
		*max_version = 0;
		*max_size = 0;
		return;
	}
	*max_version = venus->cap_version;
	*max_size = venus->cap_size;
}

uint32_t np_venus_fill_caps(np_venus *venus, uint32_t version, void *buffer, uint32_t buffer_size) {
	if (!venus || !venus->live || !buffer || buffer_size < venus->cap_size) return 0;
	memset(buffer, 0, buffer_size);
	venus->virgl.fill_caps(NP_VENUS_CAPSET_VENUS, version, buffer);
	return venus->cap_size;
}

int np_venus_context_create(np_venus *venus, uint32_t ctx_id, uint32_t capset_id,
                            const char *name) {
	if (!venus || !venus->live) return 0;
	if (capset_id != NP_VENUS_CAPSET_VENUS) return -1;
	const char *label = name ? name : "";
	return venus->virgl.context_create_with_flags(
		ctx_id, capset_id & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK,
		(uint32_t)strlen(label), label);
}

void np_venus_context_destroy(np_venus *venus, uint32_t ctx_id) {
	if (!venus || !venus->live) return;
	venus->virgl.context_destroy(ctx_id);
}

int np_venus_create_blob(np_venus *venus, uint32_t ctx_id, np_venus_blob *blob) {
	if (!venus || !blob || blob->size == 0) return -1;

	struct imported *entry = calloc(1, sizeof(*entry));
	if (!entry) return -1;
	entry->blob = *blob;

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

	struct virgl_renderer_resource_create_blob_args args;
	memset(&args, 0, sizeof(args));
	args.res_handle = blob->resource_id;
	args.ctx_id = ctx_id;
	args.blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D;
	args.blob_flags = blob->blob_flags ? blob->blob_flags
	                                   : VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE;
	args.blob_id = blob->blob_id;
	args.size = blob->size;
	int rc = venus->virgl.resource_create_blob(&args);
	if (rc != 0) {
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
	// DEVICE_LOCAL allocations are intentionally Metal-private. Asking vkr to
	// map every blob anyway makes MoltenVK take a failing vkMapMemory path for
	// each swapchain image (and for every resize). Only MAPPABLE blobs can have
	// guest aperture pages.
	if (venus->virgl.resource_map &&
	    (args.blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE)) {
		void *map = NULL;
		uint64_t map_size = 0;
		if (venus->virgl.resource_map(blob->resource_id, &map, &map_size) == 0 && map) {
			blob->pointer = map;
			if (map_size > 0) blob->size = map_size;
			entry->blob.pointer = map;
			entry->blob.size = blob->size;
			entry->mapped_by_renderer = true;
			note("blob res=%u mapped by vkr at %p size=%llu",
			     blob->resource_id, map, (unsigned long long)blob->size);
		} else {
			note("blob res=%u not CPU-mappable (Metal heap / DEVICE_LOCAL)",
			     blob->resource_id);
		}
	}
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
			if (hit->mapped_by_renderer && venus->virgl.resource_unmap) {
				venus->virgl.resource_unmap(resource_id);
			}
			if (hit->mtl_texture) {
				if (venus->virgl.release_handle_for_scanout && hit->mtl_handle_type) {
					venus->virgl.release_handle_for_scanout(
						hit->mtl_handle_type, hit->mtl_texture);
				} else {
					CFRelease(hit->mtl_texture);
				}
			}
			free(hit);
			if (venus->live) venus->virgl.resource_unref(resource_id);
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
	enum { NP_VIRGL_NATIVE_HANDLE_NONE = 0, NP_VIRGL_NATIVE_HANDLE_METAL_TEXTURE = 2 };

	pthread_mutex_lock(&venus->lock);
	for (struct imported *cur = venus->blobs; cur; cur = cur->next) {
		if (cur->blob.resource_id != resource_id) continue;
		if (cur->mtl_texture && width <= cur->mtl_width &&
		    height <= cur->mtl_height && stride == cur->mtl_stride &&
		    virgl_format == cur->mtl_format) {
			void *tex = cur->mtl_texture;
			pthread_mutex_unlock(&venus->lock);
			return tex;
		}
		if (cur->mtl_texture) {
			if (venus->virgl.release_handle_for_scanout && cur->mtl_handle_type)
				venus->virgl.release_handle_for_scanout(
					cur->mtl_handle_type, cur->mtl_texture);
			else
				CFRelease(cur->mtl_texture);
			cur->mtl_texture = NULL;
			cur->mtl_handle_type = 0;
		}
		if (!venus->virgl.create_handle_for_scanout || !width || !height) {
			pthread_mutex_unlock(&venus->lock);
			return NULL;
		}
		void *handle = NULL;
		int type = venus->virgl.create_handle_for_scanout(
			resource_id, width, height, virgl_format, 0, stride, 0, &handle);
		if (type == NP_VIRGL_NATIVE_HANDLE_METAL_TEXTURE && handle) {
			cur->mtl_texture = handle;
			cur->mtl_handle_type = type;
			cur->mtl_width = width;
			cur->mtl_height = height;
			cur->mtl_stride = stride;
			cur->mtl_format = virgl_format;
			note("blob res=%u scanout metal texture %p %ux%u stride=%u fmt=%u",
			     resource_id, handle, width, height, stride, virgl_format);
			pthread_mutex_unlock(&venus->lock);
			return handle;
		}
		note("blob res=%u create_handle_for_scanout type=%d handle=%p",
		     resource_id, type, handle);
		pthread_mutex_unlock(&venus->lock);
		return NULL;
	}
	pthread_mutex_unlock(&venus->lock);
	return NULL;
}

int np_venus_attach(np_venus *venus, uint32_t ctx_id, uint32_t resource_id) {
	if (!venus || !venus->live) return 0;
	venus->virgl.ctx_attach_resource((int)ctx_id, (int)resource_id);
	return 0;
}

int np_venus_detach(np_venus *venus, uint32_t ctx_id, uint32_t resource_id) {
	if (!venus || !venus->live) return 0;
	venus->virgl.ctx_detach_resource((int)ctx_id, (int)resource_id);
	return 0;
}

void np_venus_poll(np_venus *venus) {
	if (!venus || !venus->live || !venus->virgl.poll) return;
	venus->virgl.poll();
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

	int rc = venus->virgl.submit_cmd((void *)payload, (int)ctx_id, (int)(byte_count / 4));
	if (rc != 0) {
		if (wants_fence) finish_context_fence(venus, ctx_id, ring_idx, fence_id, false);
		else done(user, fence_id, false);
		return rc;
	}

	if (wants_fence) {
		if (venus->virgl.context_create_fence) {
			// The ring index picks the timeline: 0 retires on decode, a
			// queue ring retires when MoltenVK finished the work. Passing
			// the guest's ring through is what makes the fence truthful.
			int frc = venus->virgl.context_create_fence(
				ctx_id, VIRGL_RENDERER_FENCE_FLAG_MERGEABLE, ring_idx, fence_id);
			if (frc != 0) {
				note("context_create_fence ring=%u id=%llu failed rc=%d; retiring now",
				     ring_idx, (unsigned long long)fence_id, frc);
				finish_context_fence(venus, ctx_id, ring_idx, fence_id, false);
			}
		} else {
			// No fence API — the work is queued; poll once and complete.
			venus->virgl.poll();
			finish_context_fence(venus, ctx_id, ring_idx, fence_id, false);
		}
	} else {
		done(user, fence_id, true);
	}
	return 0;
}
