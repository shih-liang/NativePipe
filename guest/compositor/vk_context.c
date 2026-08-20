#define _GNU_SOURCE

#include "vk_context.h"

#include <stdio.h>
#include <string.h>
#include <sys/random.h>

static bool pick_physical_device(struct np_vk_context *ctx)
{
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(ctx->instance, &count, NULL) != VK_SUCCESS || count == 0)
        return false;

    VkPhysicalDevice devices[8];
    if (count > 8)
        count = 8;

    if (vkEnumeratePhysicalDevices(ctx->instance, &count, devices) != VK_SUCCESS)
        return false;

    ctx->physical_device = devices[0];
    return true;
}

static bool pick_queue(struct np_vk_context *ctx)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &count, NULL);
    if (!count)
        return false;

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
    return false;
}

static bool make_engine_name(char out[64])
{
    unsigned char token[16];
    if (getrandom(token, sizeof(token), 0) != (ssize_t)sizeof(token))
        return false;

    static const char hex[] = "0123456789abcdef";
    const char *prefix = "NativePipeDisplay/1/";
    size_t n = strlen(prefix);
    memcpy(out, prefix, n);
    for (size_t i = 0; i < sizeof(token); i++) {
        out[n + i * 2] = hex[token[i] >> 4];
        out[n + i * 2 + 1] = hex[token[i] & 15];
    }
    out[n + sizeof(token) * 2] = '\0';
    return true;
}

bool np_vk_context_init(struct np_vk_context *ctx)
{
    if (!ctx)
        return false;
    memset(ctx, 0, sizeof(*ctx));

    char engine_name[64];
    if (!make_engine_name(engine_name)) {
        fprintf(stderr, "[vk-context] could not generate display context token\n");
        return false;
    }

    /* The host vkr backend recognizes this application/token pair while
     * decoding vkCreateInstance and grants IOSurface allocation only to this
     * Venus context. The token prevents accidental matches by normal clients. */
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vmpipe-wayland-gpu-blit",
        .applicationVersion = 1,
        .pEngineName = engine_name,
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };

    if (vkCreateInstance(&instance_info, NULL, &ctx->instance) != VK_SUCCESS)
        return false;
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

    if (vkCreateDevice(ctx->physical_device, &device_info, NULL, &ctx->device) != VK_SUCCESS)
        goto fail_instance;

    vkGetDeviceQueue(ctx->device, ctx->graphics_queue_family, 0, &ctx->graphics_queue);
    fprintf(stderr, "[vk-context] compositor Venus device initialized (%s)\n", engine_name);
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
