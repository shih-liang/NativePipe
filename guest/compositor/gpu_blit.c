#define _GNU_SOURCE

#include "gpu_blit.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/virtgpu_drm.h>

#include "vulkan_core.h"

#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_ABGR8888 0x34324241u
#define DRM_FORMAT_XBGR8888 0x34324258u

#ifndef VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA
#define VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA ((VkStructureType)1000384002)
#endif

typedef struct VkImportMemoryResourceInfoMESA {
	VkStructureType sType;
	const void *pNext;
	uint32_t resourceId;
} VkImportMemoryResourceInfoMESA;

#ifndef VK_STRUCTURE_TYPE_MEMORY_RESOURCE_ALLOCATION_SIZE_PROPERTIES_MESA
#define VK_STRUCTURE_TYPE_MEMORY_RESOURCE_ALLOCATION_SIZE_PROPERTIES_MESA \
	((VkStructureType)1000384003)
#endif

typedef struct VkMemoryResourceAllocationSizePropertiesMESA {
	VkStructureType sType;
	void *pNext;
	uint64_t allocationSize;
} VkMemoryResourceAllocationSizePropertiesMESA;

#ifndef VK_STRUCTURE_TYPE_MEMORY_RESOURCE_PROPERTIES_MESA
#define VK_STRUCTURE_TYPE_MEMORY_RESOURCE_PROPERTIES_MESA ((VkStructureType)1000384001)
#endif

typedef struct VkMemoryResourcePropertiesMESA {
	VkStructureType sType;
	void *pNext;
	uint32_t memoryTypeBits;
} VkMemoryResourcePropertiesMESA;

typedef VkResult (VKAPI_PTR *PFN_vkGetMemoryResourcePropertiesMESA)(
	VkDevice device, uint32_t resourceId,
	VkMemoryResourcePropertiesMESA *pMemoryResourceProperties);

static void *vulkan_lib;
static PFN_vkGetInstanceProcAddr pfn_get_instance_proc_addr;

struct np_gpu_blit_ctx {
	int drm_fd;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t queue_family;
	VkCommandPool command_pool;
	VkCommandBuffer command;
	VkFence fence;
	PFN_vkCreateInstance CreateInstance;
	PFN_vkDestroyInstance DestroyInstance;
	PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
	PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
	PFN_vkCreateDevice CreateDevice;
	PFN_vkDestroyDevice DestroyDevice;
	PFN_vkGetDeviceQueue GetDeviceQueue;
	PFN_vkCreateCommandPool CreateCommandPool;
	PFN_vkDestroyCommandPool DestroyCommandPool;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
	PFN_vkCreateFence CreateFence;
	PFN_vkDestroyFence DestroyFence;
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkCreateImage CreateImage;
	PFN_vkDestroyImage DestroyImage;
	PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
	PFN_vkBindImageMemory BindImageMemory;
	PFN_vkWaitForFences WaitForFences;
	PFN_vkResetFences ResetFences;
	PFN_vkResetCommandBuffer ResetCommandBuffer;
	PFN_vkBeginCommandBuffer BeginCommandBuffer;
	PFN_vkEndCommandBuffer EndCommandBuffer;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
	PFN_vkCmdCopyImage CmdCopyImage;
	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR;
	PFN_vkGetMemoryResourcePropertiesMESA GetMemoryResourcePropertiesMESA;
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
	PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
};

static struct np_gpu_blit_ctx blit_ctx;

#define vkCreateInstance blit_ctx.CreateInstance
#define vkDestroyInstance blit_ctx.DestroyInstance
#define vkEnumeratePhysicalDevices blit_ctx.EnumeratePhysicalDevices
#define vkGetPhysicalDeviceQueueFamilyProperties blit_ctx.GetPhysicalDeviceQueueFamilyProperties
#define vkGetPhysicalDeviceMemoryProperties blit_ctx.GetPhysicalDeviceMemoryProperties
#define vkCreateDevice blit_ctx.CreateDevice
#define vkDestroyDevice blit_ctx.DestroyDevice
#define vkGetDeviceQueue blit_ctx.GetDeviceQueue
#define vkCreateCommandPool blit_ctx.CreateCommandPool
#define vkDestroyCommandPool blit_ctx.DestroyCommandPool
#define vkAllocateCommandBuffers blit_ctx.AllocateCommandBuffers
#define vkCreateFence blit_ctx.CreateFence
#define vkDestroyFence blit_ctx.DestroyFence
#define vkAllocateMemory blit_ctx.AllocateMemory
#define vkFreeMemory blit_ctx.FreeMemory
#define vkCreateImage blit_ctx.CreateImage
#define vkDestroyImage blit_ctx.DestroyImage
#define vkGetImageMemoryRequirements blit_ctx.GetImageMemoryRequirements
#define vkBindImageMemory blit_ctx.BindImageMemory
#define vkWaitForFences blit_ctx.WaitForFences
#define vkResetFences blit_ctx.ResetFences
#define vkResetCommandBuffer blit_ctx.ResetCommandBuffer
#define vkBeginCommandBuffer blit_ctx.BeginCommandBuffer
#define vkEndCommandBuffer blit_ctx.EndCommandBuffer
#define vkCmdPipelineBarrier blit_ctx.CmdPipelineBarrier
#define vkCmdCopyImage blit_ctx.CmdCopyImage
#define vkQueueSubmit blit_ctx.QueueSubmit
#define vkEnumerateDeviceExtensionProperties blit_ctx.EnumerateDeviceExtensionProperties

static bool load_vulkan(void) {
	if (pfn_get_instance_proc_addr) return true;
	const char *names[] = {
		"/usr/lib/libvulkan.so.1",
		"libvulkan.so.1",
		"libvulkan.so",
		NULL,
	};
	for (int i = 0; names[i]; i++) {
		vulkan_lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
		if (vulkan_lib) break;
	}
	if (!vulkan_lib) {
		fprintf(stderr, "[gpu-blit] dlopen libvulkan: %s\n", dlerror());
		return false;
	}
	pfn_get_instance_proc_addr = (PFN_vkGetInstanceProcAddr)dlsym(
		vulkan_lib, "vkGetInstanceProcAddr");
	if (!pfn_get_instance_proc_addr) {
		fprintf(stderr, "[gpu-blit] missing vkGetInstanceProcAddr\n");
		return false;
	}
#define LOAD(name) \
	do { \
		blit_ctx.name = (PFN_vk##name)dlsym(vulkan_lib, "vk" #name); \
		if (!blit_ctx.name) \
			blit_ctx.name = (PFN_vk##name)pfn_get_instance_proc_addr( \
				VK_NULL_HANDLE, "vk" #name); \
		if (!blit_ctx.name) { \
			fprintf(stderr, "[gpu-blit] missing vk" #name "\n"); \
			return false; \
		} \
	} while (0)
	LOAD(CreateInstance);
	LOAD(DestroyInstance);
	LOAD(EnumeratePhysicalDevices);
	LOAD(EnumerateDeviceExtensionProperties);
	LOAD(GetPhysicalDeviceQueueFamilyProperties);
	LOAD(GetPhysicalDeviceMemoryProperties);
	LOAD(CreateDevice);
	LOAD(DestroyDevice);
	LOAD(GetDeviceQueue);
	LOAD(CreateCommandPool);
	LOAD(DestroyCommandPool);
	LOAD(AllocateCommandBuffers);
	LOAD(CreateFence);
	LOAD(DestroyFence);
	LOAD(AllocateMemory);
	LOAD(FreeMemory);
	LOAD(CreateImage);
	LOAD(DestroyImage);
	LOAD(GetImageMemoryRequirements);
	LOAD(BindImageMemory);
	LOAD(WaitForFences);
	LOAD(ResetFences);
	LOAD(ResetCommandBuffer);
	LOAD(BeginCommandBuffer);
	LOAD(EndCommandBuffer);
	LOAD(CmdPipelineBarrier);
	LOAD(CmdCopyImage);
	LOAD(QueueSubmit);
	LOAD(GetDeviceProcAddr);
#undef LOAD
	return true;
}

static bool load_device_extensions(void) {
	blit_ctx.GetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)
		blit_ctx.GetDeviceProcAddr(blit_ctx.device, "vkGetMemoryFdPropertiesKHR");
	blit_ctx.GetMemoryResourcePropertiesMESA = (PFN_vkGetMemoryResourcePropertiesMESA)
		blit_ctx.GetDeviceProcAddr(blit_ctx.device,
		                           "vkGetMemoryResourcePropertiesMESA");
	if (!blit_ctx.GetMemoryResourcePropertiesMESA && vulkan_lib) {
		blit_ctx.GetMemoryResourcePropertiesMESA =
			(PFN_vkGetMemoryResourcePropertiesMESA)dlsym(
				vulkan_lib, "vkGetMemoryResourcePropertiesMESA");
	}
	if (!blit_ctx.GetMemoryResourcePropertiesMESA) {
		static void *venus_icd;
		const char *icd_libs[] = {
			"/opt/nativepipe/mesa-26.1.6/lib/libvulkan_virtio.so",
			"/usr/lib/libvulkan_virtio.so",
			NULL,
		};
		for (int i = 0; icd_libs[i]; i++) {
			if (!venus_icd)
				venus_icd = dlopen(icd_libs[i], RTLD_NOW | RTLD_LOCAL);
			if (venus_icd) break;
		}
		if (venus_icd) {
			blit_ctx.GetMemoryResourcePropertiesMESA =
				(PFN_vkGetMemoryResourcePropertiesMESA)dlsym(
					venus_icd, "vkGetMemoryResourcePropertiesMESA");
		}
	}
	if (!blit_ctx.GetMemoryFdPropertiesKHR) {
		fprintf(stderr, "[gpu-blit] vkGetMemoryFdPropertiesKHR missing\n");
		return false;
	}
	if (!blit_ctx.GetMemoryResourcePropertiesMESA) {
		fprintf(stderr,
		        "[gpu-blit] vkGetMemoryResourcePropertiesMESA missing; "
		        "will probe memory types for blob bind\n");
	}
	return true;
}

static bool trace_enabled(void) {
	static int enabled = -1;
	if (enabled < 0) enabled = getenv("NP_TRACE") != NULL;
	return enabled == 1;
}

static bool check_vk(VkResult result, const char *expr) {
	if (result == VK_SUCCESS) return true;
	fprintf(stderr, "[gpu-blit] %s failed: %d\n", expr, result);
	return false;
}

#define CHECK(expr) check_vk((expr), #expr)

static bool extension_supported(VkPhysicalDevice phys, const char *name) {
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(phys, NULL, &count, NULL);
	VkExtensionProperties *props = calloc(count, sizeof(*props));
	if (!props) return false;
	vkEnumerateDeviceExtensionProperties(phys, NULL, &count, props);
	bool found = false;
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(props[i].extensionName, name) == 0) {
			found = true;
			break;
		}
	}
	free(props);
	return found;
}

static VkFormat drm_format_to_vk(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		return VK_FORMAT_B8G8R8A8_UNORM;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		return VK_FORMAT_R8G8B8A8_UNORM;
	default:
		return VK_FORMAT_B8G8R8A8_UNORM;
	}
}

struct np_src_image {
	VkImage image;
	VkDeviceMemory memory;
	VkFormat format;
	int32_t width;
	int32_t height;
	uint32_t resource_id;
};

static void destroy_src_image(struct np_src_image *src) {
	if (!src) return;
	if (src->memory != VK_NULL_HANDLE)
		vkFreeMemory(blit_ctx.device, src->memory, NULL);
	if (src->image != VK_NULL_HANDLE)
		vkDestroyImage(blit_ctx.device, src->image, NULL);
	memset(src, 0, sizeof(*src));
}

static bool import_src_from_dmabuf(struct np_gpu_buffer *gpu, int dmabuf_fd,
                                   struct np_src_image *out) {
	memset(out, 0, sizeof(*out));
	out->format = drm_format_to_vk(gpu->format);
	out->width = gpu->width;
	out->height = gpu->height;
	out->resource_id = gpu->resource_id;

	int fd = fcntl(dmabuf_fd, F_DUPFD_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, "[gpu-blit] dup dmabuf fd: %s\n", strerror(errno));
		return false;
	}

	VkMemoryFdPropertiesKHR fd_props = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
	};
	if (!CHECK(blit_ctx.GetMemoryFdPropertiesKHR(
		    blit_ctx.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd,
		    &fd_props))) {
		close(fd);
		return false;
	}

	uint32_t memory_type_index = UINT32_MAX;
	VkPhysicalDeviceMemoryProperties memory_props;
	vkGetPhysicalDeviceMemoryProperties(blit_ctx.physical, &memory_props);
	for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++) {
		if (fd_props.memoryTypeBits & (1u << i)) {
			memory_type_index = i;
			break;
		}
	}
	if (memory_type_index == UINT32_MAX) {
		fprintf(stderr, "[gpu-blit] no memory type for dmabuf\n");
		close(fd);
		return false;
	}

	VkExternalMemoryImageCreateInfo external_image = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &external_image,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = out->format,
		.extent = {(uint32_t)out->width, (uint32_t)out->height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_LINEAR,
		.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (!CHECK(vkCreateImage(blit_ctx.device, &image_info, NULL, &out->image))) {
		close(fd);
		return false;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(blit_ctx.device, out->image, &requirements);

	VkImportMemoryFdInfoKHR import_info = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.fd = fd,
	};
	VkMemoryAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &import_info,
		.allocationSize = requirements.size,
		.memoryTypeIndex = memory_type_index,
	};
	if (!CHECK(vkAllocateMemory(blit_ctx.device, &alloc_info, NULL, &out->memory))) {
		destroy_src_image(out);
		close(fd);
		return false;
	}
	if (!CHECK(vkBindImageMemory(blit_ctx.device, out->image, out->memory, 0))) {
		destroy_src_image(out);
		return false;
	}
	return true;
}

struct np_dst_image {
	VkImage image;
	VkDeviceMemory memory;
	bool bound;
	bool owns_vulkan;
	uint32_t resource_id;
	uint32_t width;
	uint32_t height;
};

static struct np_dst_image dst_cache[2];
static struct np_dst_image window_buffers[2];

static void destroy_dst_image(struct np_dst_image *dst) {
	if (!dst) return;
	if (dst->owns_vulkan) {
		if (dst->memory != VK_NULL_HANDLE)
			vkFreeMemory(blit_ctx.device, dst->memory, NULL);
		if (dst->image != VK_NULL_HANDLE)
			vkDestroyImage(blit_ctx.device, dst->image, NULL);
	}
	memset(dst, 0, sizeof(*dst));
}

static struct np_dst_image *window_buffer_slot(uint32_t resource_id, uint32_t width,
                                               uint32_t height) {
	for (int i = 0; i < 2; i++) {
		struct np_dst_image *slot = &window_buffers[i];
		if (slot->resource_id == resource_id && slot->width == width &&
		    slot->height == height)
			return slot;
	}
	return NULL;
}

static struct np_dst_image *dst_slot_for(struct np_blob *blob, uint32_t width,
                                         uint32_t height, VkFormat format) {
	struct np_dst_image *window = window_buffer_slot(blob->resource_id, width, height);
	if (window) return window;

	for (int i = 0; i < 2; i++) {
		struct np_dst_image *slot = &dst_cache[i];
		if (slot->bound && slot->resource_id == blob->resource_id &&
		    slot->width == width && slot->height == height)
			return slot;
	}
	for (int i = 0; i < 2; i++) {
		struct np_dst_image *slot = &dst_cache[i];
		if (!slot->bound) {
			VkImageCreateInfo image_info = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				.imageType = VK_IMAGE_TYPE_2D,
				.format = format,
				.extent = {width, height, 1},
				.mipLevels = 1,
				.arrayLayers = 1,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.tiling = VK_IMAGE_TILING_LINEAR,
				.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};
			if (!CHECK(vkCreateImage(blit_ctx.device, &image_info, NULL,
			                         &slot->image)))
				return NULL;

			VkMemoryResourceAllocationSizePropertiesMESA size_props = {
				.sType =
					VK_STRUCTURE_TYPE_MEMORY_RESOURCE_ALLOCATION_SIZE_PROPERTIES_MESA,
			};
			VkMemoryResourcePropertiesMESA res_props = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_RESOURCE_PROPERTIES_MESA,
				.pNext = &size_props,
			};
			uint32_t memory_type_index = UINT32_MAX;
			uint64_t alloc_size = blob->size;
			VkPhysicalDeviceMemoryProperties memory_props;
			vkGetPhysicalDeviceMemoryProperties(blit_ctx.physical, &memory_props);

			if (blit_ctx.GetMemoryResourcePropertiesMESA &&
			    CHECK(blit_ctx.GetMemoryResourcePropertiesMESA(
				    blit_ctx.device, blob->resource_id, &res_props))) {
				for (uint32_t t = 0; t < memory_props.memoryTypeCount; t++) {
					if (res_props.memoryTypeBits & (1u << t)) {
						memory_type_index = t;
						break;
					}
				}
				if (size_props.allocationSize != 0)
					alloc_size = size_props.allocationSize;
			} else {
				VkMemoryRequirements mem_req;
				vkGetImageMemoryRequirements(blit_ctx.device, slot->image,
				                             &mem_req);
				if (alloc_size < mem_req.size) alloc_size = mem_req.size;
				for (uint32_t t = 0; t < memory_props.memoryTypeCount; t++) {
					if (!(mem_req.memoryTypeBits & (1u << t))) continue;
					memory_type_index = t;
					break;
				}
			}
			if (memory_type_index == UINT32_MAX) {
				fprintf(stderr, "[gpu-blit] blob res=%u has no memory type\n",
				        blob->resource_id);
				destroy_dst_image(slot);
				return NULL;
			}

			VkImportMemoryResourceInfoMESA import_res = {
				.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA,
				.resourceId = blob->resource_id,
			};
			VkMemoryAllocateInfo alloc_info = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.pNext = &import_res,
				.allocationSize = alloc_size,
				.memoryTypeIndex = memory_type_index,
			};
			if (!CHECK(vkAllocateMemory(blit_ctx.device, &alloc_info, NULL,
			                           &slot->memory))) {
				destroy_dst_image(slot);
				return NULL;
			}
			if (!CHECK(vkBindImageMemory(blit_ctx.device, slot->image, slot->memory,
			                             0))) {
				destroy_dst_image(slot);
				return NULL;
			}

			slot->bound = true;
			slot->resource_id = blob->resource_id;
			slot->width = width;
			slot->height = height;
			if (trace_enabled()) {
				fprintf(stderr,
				        "[gpu-blit] bind blob res=%u -> VkImage (bind mode)\n",
				        blob->resource_id);
			}
			return slot;
		}
	}
	fprintf(stderr, "[gpu-blit] dst cache full (two blobs already bound)\n");
	return NULL;
}

static void invalidate_dst_cache(uint32_t resource_id) {
	for (int i = 0; i < 2; i++) {
		if (dst_cache[i].bound && dst_cache[i].resource_id == resource_id)
			destroy_dst_image(&dst_cache[i]);
	}
}

static struct np_dst_image *alloc_window_buffer_slot(void) {
	for (int i = 0; i < 2; i++) {
		if (!window_buffers[i].resource_id) return &window_buffers[i];
	}
	return NULL;
}

bool np_gpu_window_buffer_create(int drm_fd, size_t size, uint32_t width, uint32_t height,
                                 uint32_t stride, struct np_blob *out) {
	memset(out, 0, sizeof(*out));
	if (blit_ctx.device == VK_NULL_HANDLE) return false;

	struct np_dst_image *slot = alloc_window_buffer_slot();
	if (!slot) return false;

	VkExternalMemoryImageCreateInfo ext_image = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
	};
	VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &ext_image,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_B8G8R8A8_UNORM,
		.extent = {width, height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_LINEAR,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (!CHECK(vkCreateImage(blit_ctx.device, &image_info, NULL, &slot->image)))
		return false;
	slot->owns_vulkan = true;

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(blit_ctx.device, slot->image, &requirements);
	if (size < requirements.size) size = requirements.size;
	/* Host aperture uses a 16 KiB granule (align-host-blob / virtio page). */
	size = (size + 16383u) & ~(size_t)16383u;

	VkExportMemoryAllocateInfo export_info = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
	};
	VkMemoryDedicatedAllocateInfo dedicated = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.pNext = &export_info,
		.image = slot->image,
	};
	VkPhysicalDeviceMemoryProperties memory_props;
	vkGetPhysicalDeviceMemoryProperties(blit_ctx.physical, &memory_props);
	uint32_t memory_type_index = UINT32_MAX;
	for (uint32_t t = 0; t < memory_props.memoryTypeCount; t++) {
		if (!(requirements.memoryTypeBits & (1u << t))) continue;
		if (!(memory_props.memoryTypes[t].propertyFlags &
		      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			continue;
		memory_type_index = t;
		break;
	}
	if (memory_type_index == UINT32_MAX) {
		fprintf(stderr, "[gpu-blit] no host-visible memory type for window buffer\n");
		destroy_dst_image(slot);
		return false;
	}

	VkMemoryAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &dedicated,
		.allocationSize = size,
		.memoryTypeIndex = memory_type_index,
	};
	if (!CHECK(vkAllocateMemory(blit_ctx.device, &alloc_info, NULL, &slot->memory))) {
		destroy_dst_image(slot);
		return false;
	}
	if (!CHECK(vkBindImageMemory(blit_ctx.device, slot->image, slot->memory, 0))) {
		destroy_dst_image(slot);
		return false;
	}

	struct drm_virtgpu_resource_create_blob create;
	memset(&create, 0, sizeof(create));
	create.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
	create.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
	create.size = size;
	create.blob_id = (uint64_t)(uintptr_t)slot->memory;
	if (ioctl(drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &create) < 0) {
		fprintf(stderr, "[gpu-blit] window CREATE_BLOB: %s\n", strerror(errno));
		destroy_dst_image(slot);
		return false;
	}

	struct drm_virtgpu_map map;
	memset(&map, 0, sizeof(map));
	map.handle = create.bo_handle;
	if (ioctl(drm_fd, DRM_IOCTL_VIRTGPU_MAP, &map) < 0) {
		fprintf(stderr, "[gpu-blit] window VIRTGPU_MAP: %s\n", strerror(errno));
		goto fail_blob;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd,
	                  (off_t)map.offset);
	if (data == MAP_FAILED) {
		fprintf(stderr, "[gpu-blit] window mmap: %s\n", strerror(errno));
		goto fail_blob;
	}

	slot->resource_id = create.res_handle;
	slot->width = width;
	slot->height = height;
	slot->owns_vulkan = true;
	slot->bound = true;

	out->bo_handle = create.bo_handle;
	out->resource_id = create.res_handle;
	out->size = size;
	out->data = data;
	if (trace_enabled()) {
		fprintf(stderr,
		        "[gpu-blit] window buffer res=%u %ux%u stride=%u mem=%" PRIu64 "\n",
		        out->resource_id, width, height, stride,
		        (uint64_t)(uintptr_t)slot->memory);
	}
	return true;

fail_blob: {
	struct drm_gem_close close_req;
	memset(&close_req, 0, sizeof(close_req));
	close_req.handle = create.bo_handle;
	ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_req);
	destroy_dst_image(slot);
	return false;
}
}

void np_gpu_window_buffer_destroy(int drm_fd, struct np_blob *blob) {
	if (!blob) return;
	for (int i = 0; i < 2; i++) {
		if (window_buffers[i].resource_id == blob->resource_id) {
			destroy_dst_image(&window_buffers[i]);
			break;
		}
	}
	invalidate_dst_cache(blob->resource_id);
	if (blob->data) {
		munmap(blob->data, blob->size);
		blob->data = NULL;
	}
	if (blob->bo_handle) {
		struct drm_gem_close close_req;
		memset(&close_req, 0, sizeof(close_req));
		close_req.handle = blob->bo_handle;
		ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_req);
		blob->bo_handle = 0;
	}
	blob->resource_id = 0;
	blob->size = 0;
}

static bool record_copy(struct np_src_image *src, struct np_dst_image *dst,
                        const struct np_damage_region *damage) {
	if (!CHECK(vkWaitForFences(blit_ctx.device, 1, &blit_ctx.fence, VK_TRUE,
	                           UINT64_MAX)))
		return false;
	CHECK(vkResetFences(blit_ctx.device, 1, &blit_ctx.fence));
	CHECK(vkResetCommandBuffer(blit_ctx.command, 0));

	VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	if (!CHECK(vkBeginCommandBuffer(blit_ctx.command, &begin_info))) return false;

	VkImageMemoryBarrier src_barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = src->image,
		.subresourceRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
	};
	VkImageMemoryBarrier dst_barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = dst->image,
		.subresourceRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
	};
	VkImageMemoryBarrier barriers[] = {src_barrier, dst_barrier};
	vkCmdPipelineBarrier(blit_ctx.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2,
	                     barriers);

	for (uint32_t i = 0; i < damage->count; i++) {
		const struct np_box *rect = &damage->rects[i];
		if (rect->width <= 0 || rect->height <= 0) continue;
		VkImageCopy copy = {
			.srcSubresource =
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.mipLevel = 0,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},
			.srcOffset = {(int32_t)rect->x, (int32_t)rect->y, 0},
			.dstSubresource =
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.mipLevel = 0,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},
			.dstOffset = {(int32_t)rect->x, (int32_t)rect->y, 0},
			.extent = {(uint32_t)rect->width, (uint32_t)rect->height, 1},
		};
		vkCmdCopyImage(blit_ctx.command, src->image,
		               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image,
		               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	}

	VkImageMemoryBarrier present_barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = dst->image,
		.subresourceRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
	};
	vkCmdPipelineBarrier(blit_ctx.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL,
	                     1, &present_barrier);

	if (!CHECK(vkEndCommandBuffer(blit_ctx.command))) return false;

	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &blit_ctx.command,
	};
	if (!CHECK(vkQueueSubmit(blit_ctx.queue, 1, &submit_info, blit_ctx.fence)))
		return false;
	if (!CHECK(vkWaitForFences(blit_ctx.device, 1, &blit_ctx.fence, VK_TRUE,
	                           UINT64_MAX)))
		return false;
	return true;
}

bool np_gpu_blit_init(int drm_fd) {
	if (blit_ctx.device != VK_NULL_HANDLE) return true;

	memset(&blit_ctx, 0, sizeof(blit_ctx));
	blit_ctx.drm_fd = drm_fd;
	if (!load_vulkan()) return false;

	VkApplicationInfo app_info = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vmpipe-wayland-gpu-blit",
		.apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo instance_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app_info,
	};
	if (!CHECK(vkCreateInstance(&instance_info, NULL, &blit_ctx.instance)))
		return false;

	uint32_t physical_count = 0;
	CHECK(vkEnumeratePhysicalDevices(blit_ctx.instance, &physical_count, NULL));
	if (physical_count == 0) {
		fprintf(stderr, "[gpu-blit] no physical device\n");
		return false;
	}
	VkPhysicalDevice physicals[4];
	if (physical_count > 4) physical_count = 4;
	CHECK(vkEnumeratePhysicalDevices(blit_ctx.instance, &physical_count, physicals));
	blit_ctx.physical = physicals[0];

	if (!extension_supported(blit_ctx.physical,
	                       VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) ||
	    !extension_supported(blit_ctx.physical,
	                       VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
		fprintf(stderr, "[gpu-blit] missing dma-buf import extensions\n");
		np_gpu_blit_fini();
		return false;
	}

	uint32_t queue_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(blit_ctx.physical, &queue_count, NULL);
	VkQueueFamilyProperties *queue_props = calloc(queue_count, sizeof(*queue_props));
	if (!queue_props) return false;
	vkGetPhysicalDeviceQueueFamilyProperties(blit_ctx.physical, &queue_count,
	                                         queue_props);
	blit_ctx.queue_family = 0;
	for (uint32_t i = 0; i < queue_count; i++) {
		if (queue_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			blit_ctx.queue_family = i;
			break;
		}
	}
	free(queue_props);

	const char *device_extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
	};
	float priority = 1.f;
	VkDeviceQueueCreateInfo queue_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = blit_ctx.queue_family,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo device_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue_info,
		.enabledExtensionCount = 2,
		.ppEnabledExtensionNames = device_extensions,
	};
	if (!CHECK(vkCreateDevice(blit_ctx.physical, &device_info, NULL,
	                         &blit_ctx.device)))
		return false;

	vkGetDeviceQueue(blit_ctx.device, blit_ctx.queue_family, 0, &blit_ctx.queue);
	if (!load_device_extensions()) {
		np_gpu_blit_fini();
		return false;
	}

	VkCommandPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = blit_ctx.queue_family,
	};
	if (!CHECK(vkCreateCommandPool(blit_ctx.device, &pool_info, NULL,
	                               &blit_ctx.command_pool)))
		return false;

	VkCommandBufferAllocateInfo cmd_alloc = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = blit_ctx.command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (!CHECK(vkAllocateCommandBuffers(blit_ctx.device, &cmd_alloc,
	                                    &blit_ctx.command)))
		return false;

	VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (!CHECK(vkCreateFence(blit_ctx.device, &fence_info, NULL, &blit_ctx.fence)))
		return false;

	if (trace_enabled()) {
		fprintf(stderr, "[gpu-blit] Venus blit ready (drm fd %d)\n", drm_fd);
	}
	return true;
}

void np_gpu_blit_fini(void) {
	for (int i = 0; i < 2; i++) destroy_dst_image(&dst_cache[i]);
	if (blit_ctx.device) {
		if (blit_ctx.fence) vkDestroyFence(blit_ctx.device, blit_ctx.fence, NULL);
		if (blit_ctx.command_pool)
			vkDestroyCommandPool(blit_ctx.device, blit_ctx.command_pool, NULL);
		vkDestroyDevice(blit_ctx.device, NULL);
	}
	if (blit_ctx.instance) vkDestroyInstance(blit_ctx.instance, NULL);
	if (vulkan_lib) dlclose(vulkan_lib);
	vulkan_lib = NULL;
	pfn_get_instance_proc_addr = NULL;
	memset(&blit_ctx, 0, sizeof(blit_ctx));
}

bool np_gpu_blit_into_blob(struct np_gpu_buffer *src, struct np_blob *dst,
                           int32_t width, int32_t height, uint32_t dst_stride,
                           const struct np_damage_region *damage) {
	(void)dst_stride;
	if (!src || !dst || !damage || damage->count == 0) return false;
	if (blit_ctx.device == VK_NULL_HANDLE) {
		fprintf(stderr, "[gpu-blit] blit engine not initialized\n");
		return false;
	}

	int dmabuf_fd = np_gpu_buffer_export_dmabuf_fd(src);
	if (dmabuf_fd < 0) return false;

	struct np_src_image imported;
	if (!import_src_from_dmabuf(src, dmabuf_fd, &imported)) {
		close(dmabuf_fd);
		return false;
	}
	close(dmabuf_fd);

	struct np_dst_image *bound =
		dst_slot_for(dst, (uint32_t)width, (uint32_t)height, imported.format);
	if (!bound) {
		destroy_src_image(&imported);
		return false;
	}

	bool ok = record_copy(&imported, bound, damage);
	destroy_src_image(&imported);

	if (!ok && trace_enabled()) {
		fprintf(stderr, "[gpu-blit] blit failed src=%u -> blob=%u\n",
		        src->resource_id, dst->resource_id);
	}
	return ok;
}

void np_gpu_blit_invalidate_blob(uint32_t resource_id) {
	invalidate_dst_cache(resource_id);
}
