/*
 * Vulkan layer that makes vkQueuePresentKHR wait until the presented image
 * has reached its X window.
 *
 * Wine's X11 driver renders child windows (such as Fusion's 3D canvas) into
 * a redirected X window and copies that to the visible window right after the
 * host vkQueuePresentKHR returns, without waiting for the present to land
 * (win32u_vkQueuePresentKHR -> client_surface_present). The copy then often
 * picks up the previous frame. On NVIDIA the image can arrive in the window
 * even after vkWaitForPresentKHR and vkQueueWaitIdle returned.
 *
 * This layer subscribes to X Damage on every surface's window and, after each
 * present, blocks until the X server reports that the window contents
 * changed, so Wine's copy always sees the new frame.
 *
 * It uses its own XCB connection: Xlib error handlers are process-global, so
 * an Xlib connection would run Wine's error handler (and its locks) whenever
 * a tracked window disappears. An Xlib version of this layer froze Fusion.
 * XCB reports errors as events, which are simply ignored.
 */

#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <xcb/xcb.h>
#include <xcb/damage.h>
/* For the Window type in VkXlibSurfaceCreateInfoKHR only; no Xlib calls are made */
#include <X11/Xlib.h>

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define PRESENT_TIMEOUT_MS 100 /* never stall a present for longer */

#define EXPORT __attribute__((visibility("default")))

/* Dispatchable handles of one instance or device share the loader's dispatch table pointer */
static void *dispatch_key(const void *handle)
{
    return *(void **)handle;
}

struct instance_data
{
    struct instance_data *next;
    void *key;
    VkInstance instance;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkCreateXlibSurfaceKHR create_xlib_surface;
};

struct device_data
{
    struct device_data *next;
    void *key;
    VkDevice device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkQueuePresentKHR queue_present;
};

/* X window behind a VkSurfaceKHR */
struct surface_data
{
    struct surface_data *next;
    VkSurfaceKHR surface;
    Window window;
};

/* Damage tracking for the window a swapchain presents to */
struct swapchain_data
{
    struct swapchain_data *next;
    VkSwapchainKHR swapchain;
    Window window;
    xcb_damage_damage_t damage;
    unsigned long events; /* damage events received so far */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct instance_data *instances;
static struct device_data *devices;
static struct surface_data *surfaces;
static struct swapchain_data *swapchains;

/* Our own X connection, used only for damage tracking; guarded by x_lock */
static pthread_mutex_t x_lock = PTHREAD_MUTEX_INITIALIZER;
static xcb_connection_t *conn;
static uint8_t damage_event_base;
static int x_init_done;

static int debug = -1;

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void trace(const char *fmt, ...)
{
    va_list args;

    if (debug < 0) debug = getenv("FUSION4LINUX_LAYER_DEBUG") != NULL;
    if (!debug) return;
    va_start(args, fmt);
    fputs("present_sync: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

static long elapsed_ms(const struct timespec *start)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

/* Called with x_lock held */
static int x_init(void)
{
    const xcb_query_extension_reply_t *ext;
    xcb_damage_query_version_reply_t *version;

    if (x_init_done) return conn != NULL;
    x_init_done = 1;
    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn))
    {
        trace("cannot connect to the X server, damage sync disabled");
        xcb_disconnect(conn);
        conn = NULL;
        return 0;
    }
    ext = xcb_get_extension_data(conn, &xcb_damage_id);
    version = xcb_damage_query_version_reply(conn, xcb_damage_query_version(conn, XCB_DAMAGE_MAJOR_VERSION,
                                                                              XCB_DAMAGE_MINOR_VERSION), NULL);
    if (!ext || !ext->present || !version)
    {
        trace("X Damage extension missing, damage sync disabled");
        free(version);
        xcb_disconnect(conn);
        conn = NULL;
        return 0;
    }
    free(version);
    damage_event_base = ext->first_event;
    return 1;
}

/* Count pending damage events per swapchain. Called with x_lock held. */
static void x_dispatch_events(void)
{
    xcb_generic_event_t *event;

    while ((event = xcb_poll_for_event(conn)))
    {
        /* Errors (e.g. for windows Wine destroyed meanwhile) are ignored */
        if ((event->response_type & 0x7f) == damage_event_base + XCB_DAMAGE_NOTIFY)
        {
            xcb_damage_damage_t damage = ((xcb_damage_notify_event_t *)event)->damage;

            pthread_mutex_lock(&lock);
            for (struct swapchain_data *sc = swapchains; sc; sc = sc->next)
                if (sc->damage == damage) sc->events++;
            pthread_mutex_unlock(&lock);
            /* Re-arm the damage object so the next change reports again */
            xcb_damage_subtract(conn, damage, XCB_NONE, XCB_NONE);
        }
        free(event);
    }
    xcb_flush(conn);
}

static struct swapchain_data *find_swapchain(VkSwapchainKHR swapchain)
{
    struct swapchain_data *sc;

    pthread_mutex_lock(&lock);
    for (sc = swapchains; sc && sc->swapchain != swapchain; sc = sc->next);
    pthread_mutex_unlock(&lock);
    return sc;
}

static unsigned long swapchain_events(struct swapchain_data *sc)
{
    unsigned long events;

    pthread_mutex_lock(&lock);
    events = sc->events;
    pthread_mutex_unlock(&lock);
    return events;
}

/* Block until the swapchain's window reported damage beyond `seen` */
static void wait_for_damage(struct swapchain_data *sc, unsigned long seen)
{
    struct timespec start;
    long left;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;)
    {
        pthread_mutex_lock(&x_lock);
        x_dispatch_events();
        pthread_mutex_unlock(&x_lock);
        if (swapchain_events(sc) > seen) break;

        if ((left = PRESENT_TIMEOUT_MS - elapsed_ms(&start)) <= 0)
        {
            trace("window %#lx: no damage after %d ms", sc->window, PRESENT_TIMEOUT_MS);
            return;
        }
        struct pollfd pfd = {xcb_get_file_descriptor(conn), POLLIN, 0};
        poll(&pfd, 1, left < 5 ? left : 5);
    }
    if (debug > 0) trace("window %#lx: damage after %ld ms", sc->window, elapsed_ms(&start));
}

static struct instance_data *find_instance(const void *handle)
{
    struct instance_data *data;
    void *key = dispatch_key(handle);

    pthread_mutex_lock(&lock);
    for (data = instances; data && data->key != key; data = data->next);
    pthread_mutex_unlock(&lock);
    return data;
}

static struct device_data *find_device(const void *handle)
{
    struct device_data *data;
    void *key = dispatch_key(handle);

    pthread_mutex_lock(&lock);
    for (data = devices; data && data->key != key; data = data->next);
    pthread_mutex_unlock(&lock);
    return data;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo *info,
                                                           const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)info->pNext;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkCreateInstance create;
    struct instance_data *data;
    VkResult res;

    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;

    gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    if (!(create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance")))
        return VK_ERROR_INITIALIZATION_FAILED;
    if ((res = create(info, allocator, instance)) != VK_SUCCESS) return res;

    if (!(data = calloc(1, sizeof(*data)))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    data->key = dispatch_key(*instance);
    data->instance = *instance;
    data->get_instance_proc_addr = gipa;
    data->destroy_instance = (PFN_vkDestroyInstance)gipa(*instance, "vkDestroyInstance");
    data->create_xlib_surface = (PFN_vkCreateXlibSurfaceKHR)gipa(*instance, "vkCreateXlibSurfaceKHR");

    pthread_mutex_lock(&lock);
    data->next = instances;
    instances = data;
    pthread_mutex_unlock(&lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
    struct instance_data **prev, *data = NULL;
    void *key = dispatch_key(instance);

    pthread_mutex_lock(&lock);
    for (prev = &instances; *prev; prev = &(*prev)->next)
    {
        if ((*prev)->key != key) continue;
        data = *prev;
        *prev = data->next;
        break;
    }
    pthread_mutex_unlock(&lock);

    if (!data) return;
    data->destroy_instance(instance, allocator);
    free(data);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *info,
                                                                 const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    struct instance_data *data = find_instance(instance);
    struct surface_data *sd;
    VkResult res;

    if (!data || !data->create_xlib_surface) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if ((res = data->create_xlib_surface(instance, info, allocator, surface)) != VK_SUCCESS) return res;

    if ((sd = calloc(1, sizeof(*sd))))
    {
        sd->surface = *surface;
        sd->window = info->window;
        pthread_mutex_lock(&lock);
        sd->next = surfaces;
        surfaces = sd;
        pthread_mutex_unlock(&lock);
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info,
                                                         const VkAllocationCallbacks *allocator, VkDevice *device)
{
    VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)info->pNext;
    struct instance_data *instance = find_instance(physical_device);
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkCreateDevice create;
    struct device_data *data;
    VkResult res;

    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerDeviceCreateInfo *)chain->pNext;
    if (!chain || !instance) return VK_ERROR_INITIALIZATION_FAILED;

    gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    if (!(create = (PFN_vkCreateDevice)gipa(instance->instance, "vkCreateDevice")))
        return VK_ERROR_INITIALIZATION_FAILED;
    if ((res = create(physical_device, info, allocator, device)) != VK_SUCCESS) return res;

    if (!(data = calloc(1, sizeof(*data)))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    data->key = dispatch_key(*device);
    data->device = *device;
    data->get_device_proc_addr = gdpa;
    data->destroy_device = (PFN_vkDestroyDevice)gdpa(*device, "vkDestroyDevice");
    data->create_swapchain = (PFN_vkCreateSwapchainKHR)gdpa(*device, "vkCreateSwapchainKHR");
    data->destroy_swapchain = (PFN_vkDestroySwapchainKHR)gdpa(*device, "vkDestroySwapchainKHR");
    data->queue_present = (PFN_vkQueuePresentKHR)gdpa(*device, "vkQueuePresentKHR");

    pthread_mutex_lock(&lock);
    data->next = devices;
    devices = data;
    pthread_mutex_unlock(&lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator)
{
    struct device_data **prev, *data = NULL;
    void *key = dispatch_key(device);

    pthread_mutex_lock(&lock);
    for (prev = &devices; *prev; prev = &(*prev)->next)
    {
        if ((*prev)->key != key) continue;
        data = *prev;
        *prev = data->next;
        break;
    }
    pthread_mutex_unlock(&lock);

    if (!data) return;
    data->destroy_device(device, allocator);
    free(data);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                                               const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain)
{
    struct device_data *data = find_device(device);
    struct surface_data *sd;
    struct swapchain_data *sc;
    Window window = 0;
    VkResult res;

    if (!data) return VK_ERROR_DEVICE_LOST;
    if ((res = data->create_swapchain(device, info, allocator, swapchain)) != VK_SUCCESS) return res;

    pthread_mutex_lock(&lock);
    for (sd = surfaces; sd; sd = sd->next)
        if (sd->surface == info->surface) window = sd->window;
    pthread_mutex_unlock(&lock);
    if (!window || !(sc = calloc(1, sizeof(*sc)))) return VK_SUCCESS;

    pthread_mutex_lock(&x_lock);
    if (x_init())
    {
        sc->swapchain = *swapchain;
        sc->window = window;
        sc->damage = xcb_generate_id(conn);
        xcb_damage_create(conn, sc->damage, window, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
        xcb_flush(conn);
        trace("swapchain %#llx tracks window %#lx", (unsigned long long)*swapchain, window);
        pthread_mutex_lock(&lock);
        sc->next = swapchains;
        swapchains = sc;
        pthread_mutex_unlock(&lock);
    }
    else free(sc);
    pthread_mutex_unlock(&x_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                            const VkAllocationCallbacks *allocator)
{
    struct device_data *data = find_device(device);
    struct swapchain_data **prev, *sc = NULL;

    pthread_mutex_lock(&lock);
    for (prev = &swapchains; *prev; prev = &(*prev)->next)
    {
        if ((*prev)->swapchain != swapchain) continue;
        sc = *prev;
        *prev = sc->next;
        break;
    }
    pthread_mutex_unlock(&lock);

    if (sc)
    {
        pthread_mutex_lock(&x_lock);
        xcb_damage_destroy(conn, sc->damage);
        xcb_flush(conn);
        pthread_mutex_unlock(&x_lock);
        free(sc);
    }
    if (data) data->destroy_swapchain(device, swapchain, allocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    struct device_data *device = find_device(queue);
    struct swapchain_data *tracked[16];
    unsigned long seen[16];
    uint32_t count = 0;
    VkResult res;

    if (!device) return VK_ERROR_DEVICE_LOST;

    /* Note how much damage each window reported before this present */
    if (conn)
    {
        pthread_mutex_lock(&x_lock);
        x_dispatch_events();
        pthread_mutex_unlock(&x_lock);
    }
    for (uint32_t i = 0; i < info->swapchainCount && count < 16; i++)
    {
        if (!(tracked[count] = find_swapchain(info->pSwapchains[i]))) continue;
        seen[count] = swapchain_events(tracked[count]);
        count++;
    }

    res = device->queue_present(queue, info);
    if (res < VK_SUCCESS) return res;

    for (uint32_t i = 0; i < count; i++)
        wait_for_damage(tracked[i], seen[i]);
    return res;
}

static PFN_vkVoidFunction intercept_device(const char *name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)layer_DestroyDevice;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)layer_CreateSwapchainKHR;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)layer_DestroySwapchainKHR;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)layer_QueuePresentKHR;
    return NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    struct device_data *data;
    PFN_vkVoidFunction func;

    if ((func = intercept_device(name))) return func;
    if (!device || !(data = find_device(device))) return NULL;
    return data->get_device_proc_addr(device, name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    struct instance_data *data;
    PFN_vkVoidFunction func;

    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)layer_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)layer_DestroyInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)layer_CreateDevice;
    if (!strcmp(name, "vkCreateXlibSurfaceKHR")) return (PFN_vkVoidFunction)layer_CreateXlibSurfaceKHR;
    if ((func = intercept_device(name))) return func;
    if (!instance || !(data = find_instance(instance))) return NULL;
    return data->get_instance_proc_addr(instance, name);
}
