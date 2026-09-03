#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#define NATIVEPIPE_ALIGNMENT ((VkDeviceSize)16384)

struct instance_state {
    void *key;
    VkInstance instance;
    PFN_vkGetInstanceProcAddr next_gipa;
    PFN_vkDestroyInstance destroy_instance;
    struct instance_state *next;
};

struct device_state {
    void *key;
    PFN_vkGetDeviceProcAddr next_gdpa;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkDestroyDevice destroy_device;
    uint32_t host_visible_types;
    struct device_state *next;
};

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static struct instance_state *instances;
static struct device_state *devices;

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nativepipe_get_instance_proc_addr(
    VkInstance instance, const char *name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nativepipe_get_device_proc_addr(
    VkDevice device, const char *name);

static void *dispatch_key(const void *handle)
{
    return handle ? *(void *const *)handle : NULL;
}

static int trace_enabled(void)
{
    const char *value = getenv("NATIVEPIPE_GPU_TRACE");
    return value && value[0] && strcmp(value, "0") != 0;
}

static VkLayerInstanceCreateInfo *instance_chain_info(
    const VkInstanceCreateInfo *create_info)
{
    VkLayerInstanceCreateInfo *info = (VkLayerInstanceCreateInfo *)create_info->pNext;
    while (info) {
        if (info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            info->function == VK_LAYER_LINK_INFO)
            return info;
        info = (VkLayerInstanceCreateInfo *)info->pNext;
    }
    return NULL;
}

static VkLayerDeviceCreateInfo *device_chain_info(const VkDeviceCreateInfo *create_info)
{
    VkLayerDeviceCreateInfo *info = (VkLayerDeviceCreateInfo *)create_info->pNext;
    while (info) {
        if (info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            info->function == VK_LAYER_LINK_INFO)
            return info;
        info = (VkLayerDeviceCreateInfo *)info->pNext;
    }
    return NULL;
}

static struct instance_state *find_instance(void *key)
{
    for (struct instance_state *state = instances; state; state = state->next)
        if (state->key == key)
            return state;
    return NULL;
}

static struct device_state *find_device(void *key)
{
    for (struct device_state *state = devices; state; state = state->next)
        if (state->key == key)
            return state;
    return NULL;
}

static int imported_memory(const VkMemoryAllocateInfo *allocate_info)
{
    const void *item = allocate_info->pNext;
    while (item) {
        /* Vulkan pNext nodes share this prefix, but reading a concrete node
         * through VkBaseInStructure violates C strict aliasing at -O2. */
        VkBaseInStructure base;
        memcpy(&base, item, sizeof(base));
        switch (base.sType) {
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
            return 1;
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        case VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID:
            return 1;
#endif
        default:
            break;
        }
        item = base.pNext;
    }
    return 0;
}

static int align_allocation(VkDeviceSize size, VkDeviceSize *aligned)
{
    if (!size || size > UINT64_MAX - (NATIVEPIPE_ALIGNMENT - 1))
        return -1;
    *aligned = (size + NATIVEPIPE_ALIGNMENT - 1) & ~(NATIVEPIPE_ALIGNMENT - 1);
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL nativepipe_create_instance(
    const VkInstanceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkInstance *instance)
{
    VkLayerInstanceCreateInfo *chain = instance_chain_info(create_info);
    if (!chain || !chain->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_gipa =
        chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    VkResult result = create(create_info, allocator, instance);
    if (result != VK_SUCCESS)
        return result;

    struct instance_state *state = calloc(1, sizeof(*state));
    if (!state) {
        PFN_vkDestroyInstance destroy =
            (PFN_vkDestroyInstance)next_gipa(*instance, "vkDestroyInstance");
        if (destroy)
            destroy(*instance, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    state->key = dispatch_key(*instance);
    state->instance = *instance;
    state->next_gipa = next_gipa;
    state->destroy_instance =
        (PFN_vkDestroyInstance)next_gipa(*instance, "vkDestroyInstance");
    pthread_mutex_lock(&state_lock);
    state->next = instances;
    instances = state;
    pthread_mutex_unlock(&state_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL nativepipe_destroy_instance(
    VkInstance instance, const VkAllocationCallbacks *allocator)
{
    void *key = dispatch_key(instance);
    pthread_mutex_lock(&state_lock);
    struct instance_state **link = &instances;
    while (*link && (*link)->key != key)
        link = &(*link)->next;
    struct instance_state *state = *link;
    if (state)
        *link = state->next;
    pthread_mutex_unlock(&state_lock);
    if (state) {
        if (state->destroy_instance)
            state->destroy_instance(instance, allocator);
        free(state);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL nativepipe_create_device(
    VkPhysicalDevice physical_device,
    const VkDeviceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDevice *device)
{
    VkLayerDeviceCreateInfo *chain = device_chain_info(create_info);
    if (!chain || !chain->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

    pthread_mutex_lock(&state_lock);
    struct instance_state *instance = find_instance(dispatch_key(physical_device));
    pthread_mutex_unlock(&state_lock);
    if (!instance)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa =
        chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa =
        chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice create =
        (PFN_vkCreateDevice)next_gipa(instance->instance, "vkCreateDevice");
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)next_gipa(
            instance->instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!create || !get_memory_properties)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkPhysicalDeviceMemoryProperties memory_properties;
    get_memory_properties(physical_device, &memory_properties);
    uint32_t host_visible_types = 0;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
        if (memory_properties.memoryTypes[i].propertyFlags &
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            host_visible_types |= UINT32_C(1) << i;

    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    VkResult result = create(physical_device, create_info, allocator, device);
    if (result != VK_SUCCESS)
        return result;

    struct device_state *state = calloc(1, sizeof(*state));
    if (!state) {
        PFN_vkDestroyDevice destroy =
            (PFN_vkDestroyDevice)next_gdpa(*device, "vkDestroyDevice");
        if (destroy)
            destroy(*device, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    state->key = dispatch_key(*device);
    state->next_gdpa = next_gdpa;
    state->allocate_memory =
        (PFN_vkAllocateMemory)next_gdpa(*device, "vkAllocateMemory");
    state->destroy_device =
        (PFN_vkDestroyDevice)next_gdpa(*device, "vkDestroyDevice");
    state->host_visible_types = host_visible_types;
    if (!state->allocate_memory || !state->destroy_device) {
        if (state->destroy_device)
            state->destroy_device(*device, allocator);
        free(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    pthread_mutex_lock(&state_lock);
    state->next = devices;
    devices = state;
    pthread_mutex_unlock(&state_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL nativepipe_destroy_device(
    VkDevice device, const VkAllocationCallbacks *allocator)
{
    void *key = dispatch_key(device);
    pthread_mutex_lock(&state_lock);
    struct device_state **link = &devices;
    while (*link && (*link)->key != key)
        link = &(*link)->next;
    struct device_state *state = *link;
    if (state)
        *link = state->next;
    pthread_mutex_unlock(&state_lock);
    if (state) {
        state->destroy_device(device, allocator);
        free(state);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL nativepipe_allocate_memory(
    VkDevice device,
    const VkMemoryAllocateInfo *allocate_info,
    const VkAllocationCallbacks *allocator,
    VkDeviceMemory *memory)
{
    pthread_mutex_lock(&state_lock);
    struct device_state *state = find_device(dispatch_key(device));
    PFN_vkAllocateMemory allocate = state ? state->allocate_memory : NULL;
    uint32_t host_visible_types = state ? state->host_visible_types : 0;
    pthread_mutex_unlock(&state_lock);
    if (!allocate)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (allocate_info->memoryTypeIndex >= 32 ||
        !(host_visible_types & (UINT32_C(1) << allocate_info->memoryTypeIndex)) ||
        imported_memory(allocate_info))
        return allocate(device, allocate_info, allocator, memory);

    VkDeviceSize aligned;
    if (align_allocation(allocate_info->allocationSize, &aligned) < 0)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (aligned == allocate_info->allocationSize)
        return allocate(device, allocate_info, allocator, memory);

    VkMemoryAllocateInfo copy = *allocate_info;
    copy.allocationSize = aligned;
    if (trace_enabled())
        fprintf(stderr, "[align-layer] vkAllocateMemory %llu -> %llu\n",
                (unsigned long long)allocate_info->allocationSize,
                (unsigned long long)aligned);
    return allocate(device, &copy, allocator, memory);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nativepipe_get_device_proc_addr(
    VkDevice device, const char *name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)nativepipe_get_device_proc_addr;
    if (!strcmp(name, "vkAllocateMemory"))
        return (PFN_vkVoidFunction)nativepipe_allocate_memory;
    if (!strcmp(name, "vkDestroyDevice"))
        return (PFN_vkVoidFunction)nativepipe_destroy_device;
    pthread_mutex_lock(&state_lock);
    struct device_state *state = find_device(dispatch_key(device));
    PFN_vkGetDeviceProcAddr next = state ? state->next_gdpa : NULL;
    pthread_mutex_unlock(&state_lock);
    return next ? next(device, name) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nativepipe_get_instance_proc_addr(
    VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)nativepipe_get_instance_proc_addr;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)nativepipe_get_device_proc_addr;
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)nativepipe_create_instance;
    if (!strcmp(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)nativepipe_destroy_instance;
    if (!strcmp(name, "vkCreateDevice"))
        return (PFN_vkVoidFunction)nativepipe_create_device;
    if (!strcmp(name, "vkDestroyDevice"))
        return (PFN_vkVoidFunction)nativepipe_destroy_device;
    if (!strcmp(name, "vkAllocateMemory"))
        return (PFN_vkVoidFunction)nativepipe_allocate_memory;
    pthread_mutex_lock(&state_lock);
    struct instance_state *state = find_instance(dispatch_key(instance));
    PFN_vkGetInstanceProcAddr next = state ? state->next_gipa : NULL;
    pthread_mutex_unlock(&state_lock);
    return next ? next(instance, name) : NULL;
}

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL nativepipe_NegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface *version)
{
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
        version->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = nativepipe_get_instance_proc_addr;
    version->pfnGetDeviceProcAddr = nativepipe_get_device_proc_addr;
    version->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}

#ifdef NATIVEPIPE_ALIGNMENT_SELF_TEST
#include <assert.h>
int main(void)
{
    VkDeviceSize aligned = 0;
    assert(align_allocation(1, &aligned) == 0 && aligned == NATIVEPIPE_ALIGNMENT);
    assert(align_allocation(NATIVEPIPE_ALIGNMENT, &aligned) == 0 &&
           aligned == NATIVEPIPE_ALIGNMENT);
    VkImportMemoryFdInfoKHR imported = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
    };
    VkMemoryAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &imported,
        .allocationSize = 4096,
    };
    assert(imported_memory(&info));
    return 0;
}
#endif
