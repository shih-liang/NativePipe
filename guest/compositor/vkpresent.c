#define _GNU_SOURCE
// Wayland + Venus present. vkcube calls CreateSwapchain from its
// xdg_surface.configure listener; Mesa WSI then roundtrips the same
// wl_display and Alpine libwayland SIGSEGVs. This client acks configure
// first, then talks to Vulkan only from the main loop.

#include "xdg-shell-client-protocol.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#define CHECK(expr)                                                            \
	do {                                                                   \
		VkResult _r = (expr);                                          \
		if (_r != VK_SUCCESS) {                                        \
			fprintf(stderr, "%s failed: %d\n", #expr, _r);         \
			return 1;                                              \
		}                                                              \
	} while (0)

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_surface *wl_surface;
static struct xdg_surface *xdg_surf;
static struct xdg_toplevel *toplevel;
static int width = 800, height = 600;
static int configured;
static int running = 1;

/// Which step the main loop last entered, so a silent signal death still
/// names the call it happened in.
static const char *phase = "startup";

static void crash_handler(int sig) {
	// write(2) is async-signal-safe; fprintf is not.
	char message[128];
	int n = snprintf(message, sizeof(message),
	                 "vkpresent: fatal signal %d during %s\n", sig, phase);
	if (n > 0) write(2, message, (size_t)n);
	_exit(128 + sig);
}

static void install_crash_handler(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = crash_handler;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *iface, uint32_t version) {
	(void)data;
	(void)version;
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static void xdg_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(surface, serial);
	configured = 1;
}

static const struct xdg_surface_listener xdg_listener = {.configure = xdg_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *top, int32_t w, int32_t h,
                               struct wl_array *states) {
	(void)data;
	(void)top;
	(void)states;
	if (w > 0) width = w;
	if (h > 0) height = h;
}

static void toplevel_close(void *data, struct xdg_toplevel *top) {
	(void)data;
	(void)top;
	running = 0;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

int main(void) {
	install_crash_handler();
	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "wl_display_connect failed\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !wm_base) {
		fprintf(stderr, "missing compositor or xdg_wm_base\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);

	wl_surface = wl_compositor_create_surface(compositor);
	xdg_surf = xdg_wm_base_get_xdg_surface(wm_base, wl_surface);
	xdg_surface_add_listener(xdg_surf, &xdg_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surf);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "vkpresent");
	wl_surface_commit(wl_surface);
	while (!configured)
		wl_display_dispatch(display);
	fprintf(stderr, "configured %dx%d, creating swapchain\n", width, height);

	const char *inst_exts[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
	};
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkpresent",
		.apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
		.enabledExtensionCount = 2,
		.ppEnabledExtensionNames = inst_exts,
	};
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));

	uint32_t nphys = 0;
	CHECK(vkEnumeratePhysicalDevices(inst, &nphys, NULL));
	if (!nphys) {
		fprintf(stderr, "no physical devices\n");
		return 1;
	}
	VkPhysicalDevice physs[8];
	if (nphys > 8) nphys = 8;
	CHECK(vkEnumeratePhysicalDevices(inst, &nphys, physs));
	VkPhysicalDevice phys = physs[0];
	VkPhysicalDeviceProperties pprops;
	vkGetPhysicalDeviceProperties(phys, &pprops);
	fprintf(stderr, "GPU: %s\n", pprops.deviceName);

	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
	VkQueueFamilyProperties qprops[8];
	if (nq > 8) nq = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qprops);
	uint32_t qfam = 0;
	for (uint32_t i = 0; i < nq; i++) {
		if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			qfam = i;
			break;
		}
	}
	float prio = 1.f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = qfam,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};
	const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
		.enabledExtensionCount = 1,
		.ppEnabledExtensionNames = dev_exts,
	};
	VkDevice dev;
	CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
	VkQueue queue;
	vkGetDeviceQueue(dev, qfam, 0, &queue);

	VkWaylandSurfaceCreateInfoKHR sci = {
		.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
		.display = display,
		.surface = wl_surface,
	};
	VkSurfaceKHR vksurf;
	CHECK(vkCreateWaylandSurfaceKHR(inst, &sci, NULL, &vksurf));

	VkBool32 supported = VK_FALSE;
	CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(phys, qfam, vksurf, &supported));
	if (!supported) {
		fprintf(stderr, "queue family cannot present\n");
		return 1;
	}

	VkSurfaceCapabilitiesKHR caps;
	CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, vksurf, &caps));
	uint32_t nfmt = 0;
	CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, vksurf, &nfmt, NULL));
	VkSurfaceFormatKHR *fmts = calloc(nfmt ? nfmt : 1, sizeof(*fmts));
	CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, vksurf, &nfmt, fmts));
	VkSurfaceFormatKHR fmt = fmts[0];
	for (uint32_t i = 0; i < nfmt; i++) {
		if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
			fmt = fmts[i];
			break;
		}
	}
	free(fmts);
	fprintf(stderr, "surface format %u, %u formats\n", fmt.format, nfmt);

	uint32_t img_count = caps.minImageCount > 2 ? caps.minImageCount : 2;
	if (caps.maxImageCount && img_count > caps.maxImageCount)
		img_count = caps.maxImageCount;
	VkExtent2D extent = caps.currentExtent;
	if (extent.width == 0xffffffff) {
		extent.width = (uint32_t)width;
		extent.height = (uint32_t)height;
	}
	if (extent.width < caps.minImageExtent.width)
		extent.width = caps.minImageExtent.width;
	if (extent.height < caps.minImageExtent.height)
		extent.height = caps.minImageExtent.height;

	VkSwapchainCreateInfoKHR swci = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = vksurf,
		.minImageCount = img_count,
		.imageFormat = fmt.format,
		.imageColorSpace = fmt.colorSpace,
		.imageExtent = extent,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = caps.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		.clipped = VK_TRUE,
	};
	VkSwapchainKHR swapchain;
	CHECK(vkCreateSwapchainKHR(dev, &swci, NULL, &swapchain));
	fprintf(stderr, "swapchain %ux%u\n", extent.width, extent.height);

	uint32_t nimg = 0;
	CHECK(vkGetSwapchainImagesKHR(dev, swapchain, &nimg, NULL));
	VkImage *images = calloc(nimg, sizeof(*images));
	CHECK(vkGetSwapchainImagesKHR(dev, swapchain, &nimg, images));

	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = qfam,
	};
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cmd;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));

	VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	                         .flags = VK_FENCE_CREATE_SIGNALED_BIT};
	VkSemaphoreCreateInfo sei = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkFence fence;
	VkSemaphore acq, rel;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));
	CHECK(vkCreateSemaphore(dev, &sei, NULL, &acq));
	CHECK(vkCreateSemaphore(dev, &sei, NULL, &rel));

	uint32_t frame = 0;
	while (running) {
		phase = "wl_display_dispatch_pending";
		wl_display_dispatch_pending(display);
		phase = "wl_display_flush";
		wl_display_flush(display);

		phase = "vkWaitForFences";
		CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
		CHECK(vkResetFences(dev, 1, &fence));
		uint32_t idx = 0;
		phase = "vkAcquireNextImageKHR";
		VkResult ar = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
		if (ar == VK_ERROR_OUT_OF_DATE_KHR) break;
		if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
			fprintf(stderr, "AcquireNextImageKHR %d\n", ar);
			return 1;
		}

		float t = (frame % 180) / 180.f;
		VkClearColorValue color = {.float32 = {t, 0.2f, 1.f - t, 1.f}};
		VkImageSubresourceRange range = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		};
		VkImageMemoryBarrier to_dst = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = images[idx],
			.subresourceRange = range,
		};
		VkImageMemoryBarrier to_present = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = images[idx],
			.subresourceRange = range,
		};

		CHECK(vkResetCommandBuffer(cmd, 0));
		VkCommandBufferBeginInfo bi = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		CHECK(vkBeginCommandBuffer(cmd, &bi));
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
		                     &to_dst);
		vkCmdClearColorImage(cmd, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                     &color, 1, &range);
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
		                     NULL, 1, &to_present);
		CHECK(vkEndCommandBuffer(cmd));

		VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo si = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &acq,
			.pWaitDstStageMask = &wait,
			.commandBufferCount = 1,
			.pCommandBuffers = &cmd,
			.signalSemaphoreCount = 1,
			.pSignalSemaphores = &rel,
		};
		phase = "vkQueueSubmit";
		CHECK(vkQueueSubmit(queue, 1, &si, fence));
		VkPresentInfoKHR pi = {
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &rel,
			.swapchainCount = 1,
			.pSwapchains = &swapchain,
			.pImageIndices = &idx,
		};
		phase = "vkQueuePresentKHR";
		VkResult pr = vkQueuePresentKHR(queue, &pi);
		if (pr == VK_ERROR_OUT_OF_DATE_KHR) break;
		if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
			fprintf(stderr, "QueuePresentKHR %d\n", pr);
			return 1;
		}
		// Push the commit out now. Present may leave it in libwayland's
		// buffer; if this process dies before the next loop turn the
		// frame is lost and the host never creates the window.
		phase = "post-present flush";
		wl_display_flush(display);
		if (frame == 0)
			fprintf(stderr, "first present ok\n");
		if (frame < 3 || frame % 60 == 0)
			fprintf(stderr, "frame %u presented\n", frame);
		frame++;
	}
	fprintf(stderr, "vkpresent exit after %u frames\n", frame);
	return 0;
}
