#include "vk_context.h"

#include <stdio.h>
#include <string.h>

static bool pick_physical_device(struct np_vk_context *ctx)
{
    uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(ctx->instance, &count, NULL);
    if (result != VK_SUCCESS || count == 0) {
        fprintf(stderr, "[vk-context] enumerate physical devices: result=%d count=%u\n",
                result, count);
        return false;
    }

    VkPhysicalDevice devices[8];
    if (count > 8)
        count = 8;

    result = vkEnumeratePhysicalDevices(ctx->instance, &count, devices);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[vk-context] fetch physical devices: %d\n", result);
        return false;
    }

    ctx->physical_device = devices[0];
    return true;
}

static bool pick_queue(struct np_vk_context *ctx)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &count, NULL);
    if (!count) {
        fprintf(stderr, "[vk-context] no queue families\n");
        return false;
    }

    VkQueueFamilyProperties props[16];
    if (count > 16)
        count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &count, props);

    for (uint32_t i = 0; i < count; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            ctx->graphics_queue_family = i;
            return true;
        }
    }
    fprintf(stderr, "[vk-context] no graphics queue family\n");
    return false;
}

bool np_vk_context_init(struct np_vk_context *ctx)
{
    if (!ctx)
        return false;
    memset(ctx, 0, sizeof(*ctx));

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "NativePipe compositor",
        .applicationVersion = 1,
        .pEngineName = "NativePipe scene",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };

    VkResult result = vkCreateInstance(&instance_info, NULL, &ctx->instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[vk-context] vkCreateInstance failed: %d\n", result);
        return false;
    }
    if (!pick_physical_device(ctx) || !pick_queue(ctx))
        goto fail_instance;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->graphics_queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };

    const char *extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]),
        .ppEnabledExtensionNames = extensions,
    };

    result = vkCreateDevice(ctx->physical_device, &device_info, NULL, &ctx->device);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[vk-context] vkCreateDevice failed: %d\n", result);
        goto fail_instance;
    }

    vkGetDeviceQueue(ctx->device, ctx->graphics_queue_family, 0, &ctx->graphics_queue);
    fprintf(stderr, "[vk-context] compositor Venus device initialized\n");
    return true;

fail_instance:
    if (ctx->instance)
        vkDestroyInstance(ctx->instance, NULL);
    memset(ctx, 0, sizeof(*ctx));
    return false;
}

void np_vk_context_destroy(struct np_vk_context *ctx)
{
    if (!ctx)
        return;
    if (ctx->device)
        vkDestroyDevice(ctx->device, NULL);
    if (ctx->instance)
        vkDestroyInstance(ctx->instance, NULL);
    memset(ctx, 0, sizeof(*ctx));
}
