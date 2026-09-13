/* Android-surface render backend — see vk_present.h. Uses Turnip via vk_loader
 * (g_vk.*), not the process-default system Adreno driver. */
#define _POSIX_C_SOURCE 200809L
#include "vk_present.h"
#include "vk_loader.h"
#include "sc_layer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>

#define TAG "BannerWayland"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) banner_log("error", __VA_ARGS__)
#define MOD_INVALID VKP_MOD_INVALID

struct vkp_image {
    VkImage image;
    VkDeviceMemory mem;
    int w, h;
    int dmabuf;               /* imported from a client; owned by the foreign queue family */
    int blit_dst;             /* layer mode: a pool buffer this backend blits into (never a source) */
    void *map;                /* shm images: persistently mapped linear memory */
    VkDeviceSize offset, row_pitch;
    int in_general;           /* shm images: moved from PREINITIALIZED to GENERAL */
};

static ANativeWindow *g_window;  /* the window frames go to; compositor thread only */
static char *g_driver_path, *g_library_name, *g_native_lib_dir;
static int g_dev_state;       /* 0 = not yet, 1 = ok, -1 = failed, -2 = lost (VK_ERROR_DEVICE_LOST) */
static VkInstance g_inst;
static VkPhysicalDevice g_pd;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfam;
static VkCommandPool g_pool;
static VkCommandBuffer g_cmd;
static VkSemaphore g_acq, g_rnd;
static VkFence g_fence;
static VkPhysicalDeviceMemoryProperties g_memprops;

static VkSurfaceKHR g_surface;
static VkSwapchainKHR g_swapchain;
static VkImage *g_images;
static uint32_t g_nimg;
static VkExtent2D g_extent;

static int g_first_frame_done; /* one-shot: fire banner_on_first_frame() on first present */
static const char *vk_result_name(VkResult r);

/* The Android surface is created/destroyed on the app's UI thread while the compositor thread
 * may be inside a WSI call (acquire and present can block for a refresh or more). The UI thread
 * therefore never touches the swapchain: it leaves the new window here and returns at once; the
 * compositor thread picks it up before its next frame (vkp_apply_window_request). g_req_lock is
 * only ever held for these few assignments, never across a Vulkan call. */
static pthread_mutex_t g_req_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *g_req_window;
static int g_req_pending;

/* Scale mode + alignment (app values, see vk_present.h); written from any thread, read per frame. */
static volatile int g_mode = VKP_MODE_STRETCH, g_align = VKP_ALIGN_CENTER;

/* The scene -> output mapping of the last frame (compositor thread): scene pixel (x,y) lands at
 * output (off_x + x * kx, off_y + y * ky), clipped to the region rectangle. */
static struct {
    int scene_w, scene_h, out_w, out_h, mode, align;
    float kx, ky, off_x, off_y;
    int rx, ry, rw, rh;                     /* region the picture may occupy */
    int valid;
} g_map;

/* A failed swapchain creation is retried no sooner than this (the window may be mid-teardown),
 * and the failure is logged once per streak instead of every frame. */
static int64_t g_swap_retry_at_ns;
static int g_swap_fail_logged;

/* Implemented in waylandcomp_jni.c — notifies Java (dismiss launch overlay). */
extern void banner_on_first_frame(void);

static char g_gpu_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
const char *vkp_gpu_name(void) { return g_gpu_name; }

/* DRM fourccs name the channels of a little-endian 32-bit word: XRGB8888 is B,G,R,X in memory
 * (= VK B8G8R8A8) and XBGR8888 is R,G,B,X (= VK R8G8B8A8). Turnip's Wayland WSI sends XB24 for
 * R8G8B8A8 swapchains, so reading everything as BGRA swaps red and blue. */
static VkFormat drm_to_vk(uint32_t drm) {
    switch (drm) {
    case 0x34324241: /* AB24 */
    case 0x34324258: /* XB24 */
        return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x30334241: /* AB30 */
    case 0x30334258: /* XB30 */
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x30335241: /* AR30 */
    case 0x30335258: /* XR30 */
        return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    case 0x48344241: /* AB4H */
    case 0x48344258: /* XB4H */
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: /* AR24 / XR24 */
        return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

void vk_present_set_driver(const char *driver_path, const char *library_name,
                           const char *native_lib_dir) {
    free(g_driver_path); free(g_library_name); free(g_native_lib_dir);
    g_driver_path = driver_path ? strdup(driver_path) : NULL;
    g_library_name = library_name ? strdup(library_name) : NULL;
    g_native_lib_dir = native_lib_dir ? strdup(native_lib_dir) : NULL;
}

static void destroy_swapchain(void) {
    if (g_dev_state != 1) return;
    g_vk.DeviceWaitIdle(g_dev);
    if (g_swapchain) g_vk.DestroySwapchainKHR(g_dev, g_swapchain, NULL);
    g_swapchain = VK_NULL_HANDLE;
    if (g_surface) g_vk.DestroySurfaceKHR(g_inst, g_surface, NULL);
    g_surface = VK_NULL_HANDLE;
    free(g_images);
    g_images = NULL;
    g_nimg = 0;
}

void vk_present_set_window(ANativeWindow *window) {
    ANativeWindow *superseded = NULL;
    pthread_mutex_lock(&g_req_lock);
    /* Two requests before the compositor thread got to the first: the first window was never
     * used, so its reference is dropped here. */
    if (g_req_pending && g_req_window && g_req_window != window) superseded = g_req_window;
    g_req_window = window;
    g_req_pending = 1;
    pthread_mutex_unlock(&g_req_lock);
    if (superseded) ANativeWindow_release(superseded);
}

int vkp_apply_window_request(void) {
    ANativeWindow *w;
    pthread_mutex_lock(&g_req_lock);
    if (!g_req_pending) { pthread_mutex_unlock(&g_req_lock); return 0; }
    w = g_req_window;
    g_req_window = NULL;
    g_req_pending = 0;
    pthread_mutex_unlock(&g_req_lock);
    if (w == g_window) {
        /* The same window handed over again (ANativeWindow_fromSurface adds a reference each time). */
        if (w) ANativeWindow_release(w);
        return 0;
    }
    sc_layer_window_gone();  /* the layer (if any) belongs to the old window */
    destroy_swapchain(); /* recreated against the new window on the next frame */
    if (g_window) ANativeWindow_release(g_window);
    g_window = w;
    g_swap_retry_at_ns = 0;
    g_swap_fail_logged = 0;
    banner_log("gpu", w ? "screen surface attached" : "screen surface gone: presenting paused");
    return 1;
}

int vkp_has_window(void) {
    int r;
    pthread_mutex_lock(&g_req_lock);
    r = g_req_pending ? g_req_window != NULL : g_window != NULL;
    pthread_mutex_unlock(&g_req_lock);
    return r;
}

int vkp_device_lost(void) { return g_dev_state == -2; }

void vk_present_set_scale_mode(int mode, int alignment) {
    if (mode < VKP_MODE_OFF || mode > VKP_MODE_INTEGER) mode = VKP_MODE_FIT;
    if (alignment < VKP_ALIGN_CENTER || alignment > VKP_ALIGN_BOTTOM) alignment = VKP_ALIGN_CENTER;
    g_mode = mode;
    g_align = alignment;
}

static const char *mode_name(int m) {
    switch (m) {
    case VKP_MODE_OFF: return "off (letterbox)";
    case VKP_MODE_FIT: return "fit";
    case VKP_MODE_STRETCH: return "stretch";
    case VKP_MODE_FILL: return "fill";
    case VKP_MODE_INTEGER: return "integer";
    default: return "?";
    }
}
static const char *align_name(int a) {
    switch (a) {
    case VKP_ALIGN_TOP: return "top";
    case VKP_ALIGN_BOTTOM: return "bottom";
    default: return "center";
    }
}

/* Mirror of ViewTransformation.update(outer = output, inner = scene, mode, alignment) — keep the
 * arithmetic identical: the app maps touch input through that class with the same inputs, so any
 * difference here puts the pointer beside what it is pointing at. OFF and FIT are both an
 * aspect-preserving letterbox (OFF only differs in the app's fullscreen gates); TOP/BOTTOM confine
 * the picture to the top/bottom half of the output (the app's handheld split, #413). */
static void update_map(int scene_w, int scene_h) {
    int mode = g_mode, align = g_align;
    int W = (int)g_extent.width, H = (int)g_extent.height;
    if (g_map.valid && g_map.scene_w == scene_w && g_map.scene_h == scene_h && g_map.out_w == W &&
        g_map.out_h == H && g_map.mode == mode && g_map.align == align)
        return;
    g_map.scene_w = scene_w; g_map.scene_h = scene_h; g_map.out_w = W; g_map.out_h = H;
    g_map.mode = mode; g_map.align = align;

    int half = H / 2;
    switch (align) {
    case VKP_ALIGN_TOP:    g_map.rx = 0; g_map.ry = 0;    g_map.rw = W; g_map.rh = half; break;
    case VKP_ALIGN_BOTTOM: g_map.rx = 0; g_map.ry = half; g_map.rw = W; g_map.rh = H - half; break;
    default:               g_map.rx = 0; g_map.ry = 0;    g_map.rw = W; g_map.rh = H; break;
    }
    float sx = (float)g_map.rw / scene_w, sy = (float)g_map.rh / scene_h;
    if (mode == VKP_MODE_STRETCH) {
        g_map.kx = sx; g_map.ky = sy;
        g_map.off_x = (float)g_map.rx; g_map.off_y = (float)g_map.ry;
    } else {
        float aspect;
        if (mode == VKP_MODE_FILL) aspect = sx > sy ? sx : sy;
        else if (mode == VKP_MODE_INTEGER) {
            float m = sx < sy ? sx : sy;
            aspect = (float)(int)m; /* floor for m >= 1 */
            if (aspect < 1.0f) aspect = 1.0f;
        } else aspect = sx < sy ? sx : sy;
        g_map.kx = g_map.ky = aspect;
        /* Same integer truncation as ViewTransformation's viewOffsetX/Y. */
        g_map.off_x = (float)(g_map.rx + (int)((g_map.rw - scene_w * aspect) * 0.5f));
        g_map.off_y = (float)(g_map.ry + (int)((g_map.rh - scene_h * aspect) * 0.5f));
    }
    g_map.valid = 1;
    banner_log("screen", "%s, %s: %dx%d scene shown %dx%d at %d,%d on the %dx%d output",
               mode_name(mode), align_name(align), scene_w, scene_h,
               (int)(scene_w * g_map.kx + 0.5f), (int)(scene_h * g_map.ky + 0.5f),
               (int)g_map.off_x, (int)g_map.off_y, W, H);
}

int vkp_output_to_scene(double ox, double oy, double *sx, double *sy) {
    if (!g_map.valid || g_map.kx <= 0 || g_map.ky <= 0) return 0;
    *sx = (ox - g_map.off_x) / g_map.kx;
    *sy = (oy - g_map.off_y) / g_map.ky;
    return 1;
}

void vkp_output_size(int *w, int *h) {
    *w = (int)g_extent.width;
    *h = (int)g_extent.height;
}

static int has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

/* Instance + device + command objects. Doesn't need the window. */
static int dev_init(void) {
    if (g_dev_state != 0) return g_dev_state == 1 ? 0 : -1;

    /* Load Turnip (adrenotools) and its entry points — NOT the system driver. */
    if (vk_loader_open(g_driver_path, g_library_name, g_native_lib_dir) != 0) {
        LOGE("present: vk_loader_open failed"); g_dev_state = -1; return -1;
    }

    const char *inst_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                               VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "banner-wayland-present",
                             .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app,
                                .enabledExtensionCount = 2,
                                .ppEnabledExtensionNames = inst_exts};
    if (g_vk.CreateInstance(&ici, NULL, &g_inst) != VK_SUCCESS) {
        LOGE("present: vkCreateInstance failed"); g_dev_state = -1; return -1;
    }
    vk_loader_load_instance(g_inst);

    uint32_t npd = 0;
    g_vk.EnumeratePhysicalDevices(g_inst, &npd, NULL);
    if (!npd) { LOGE("present: no physical devices"); g_dev_state = -1; return -1; }
    VkPhysicalDevice pds[8]; if (npd > 8) npd = 8;
    g_vk.EnumeratePhysicalDevices(g_inst, &npd, pds);
    g_pd = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < npd && g_pd == VK_NULL_HANDLE; i++) {
        uint32_t nq = 0;
        g_vk.GetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, NULL);
        VkQueueFamilyProperties qs[16]; if (nq > 16) nq = 16;
        g_vk.GetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qs);
        for (uint32_t q = 0; q < nq; q++)
            if (qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_pd = pds[i]; g_qfam = q; break; }
    }
    if (g_pd == VK_NULL_HANDLE) { LOGE("present: no graphics queue"); g_dev_state = -1; return -1; }
    {
        VkPhysicalDeviceProperties props;
        g_vk.GetPhysicalDeviceProperties(g_pd, &props);
        snprintf(g_gpu_name, sizeof(g_gpu_name), "%s", props.deviceName);
        banner_log("gpu", "compositor renders on %s with %s", props.deviceName,
                   g_library_name ? g_library_name : "the system Vulkan driver");
        if (g_driver_path) banner_log("gpu", "driver folder %s", g_driver_path);
    }
    g_vk.GetPhysicalDeviceMemoryProperties(g_pd, &g_memprops);

    /* Verify the dmabuf-import extensions are present, and log any that are missing. */
    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_external_memory_fd",
                              "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier",
                              "VK_KHR_image_format_list"};
    uint32_t ne = 0;
    g_vk.EnumerateDeviceExtensionProperties(g_pd, NULL, &ne, NULL);
    VkExtensionProperties *exts = calloc(ne, sizeof(*exts));
    g_vk.EnumerateDeviceExtensionProperties(g_pd, NULL, &ne, exts);
    for (unsigned i = 0; i < 5; i++)
        if (!has_ext(exts, ne, dev_exts[i]))
            LOGE("present: driver MISSING %s (dmabuf import will fail)", dev_exts[i]);
    free(exts);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = g_qfam, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = 5, .ppEnabledExtensionNames = dev_exts};
    if (g_vk.CreateDevice(g_pd, &dci, NULL, &g_dev) != VK_SUCCESS) {
        LOGE("present: vkCreateDevice failed"); g_dev_state = -1; return -1;
    }
    vk_loader_load_device(g_dev);
    g_vk.GetDeviceQueue(g_dev, g_qfam, 0, &g_queue);

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g_qfam};
    g_vk.CreateCommandPool(g_dev, &pci, NULL, &g_pool);
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = g_pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    g_vk.AllocateCommandBuffers(g_dev, &cai, &g_cmd);
    VkSemaphoreCreateInfo semci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    g_vk.CreateSemaphore(g_dev, &semci, NULL, &g_acq);
    g_vk.CreateSemaphore(g_dev, &semci, NULL, &g_rnd);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    g_vk.CreateFence(g_dev, &fci, NULL, &g_fence);

    g_dev_state = 1;
    return 0;
}

int vkp_ready(void) { return dev_init(); }

static int swap_init_locked(void);

static int swap_init(void) {
    struct timespec ts;
    int64_t now;
    if (!g_window) return -1;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (now < g_swap_retry_at_ns) return -1;
    if (swap_init_locked() == 0) {
        g_swap_retry_at_ns = 0;
        g_swap_fail_logged = 0;
        return 0;
    }
    destroy_swapchain();
    g_swap_retry_at_ns = now + 500000000LL;
    if (!g_swap_fail_logged) {
        g_swap_fail_logged = 1;
        LOGE("present: no swapchain on the screen surface; retrying every 0.5 s");
    }
    return -1;
}

#define SWLOGE(...) do { if (!g_swap_fail_logged) LOGE(__VA_ARGS__); } while (0)
static int swap_init_locked(void) {

    VkAndroidSurfaceCreateInfoKHR aci = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = g_window};
    if (g_vk.CreateAndroidSurfaceKHR(g_inst, &aci, NULL, &g_surface) != VK_SUCCESS) {
        SWLOGE("present: create android surface failed"); return -1;
    }
    VkBool32 sup = VK_FALSE;
    g_vk.GetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qfam, g_surface, &sup);
    if (!sup) { SWLOGE("present: queue can't present to the window"); return -1; }

    VkSurfaceCapabilitiesKHR caps;
    g_vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surface, &caps);
    uint32_t nfmt = 0;
    g_vk.GetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surface, &nfmt, NULL);
    VkSurfaceFormatKHR fmts[32]; if (nfmt > 32) nfmt = 32;
    g_vk.GetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surface, &nfmt, fmts);
    VkSurfaceFormatKHR chosen = fmts[0];

    g_extent = caps.currentExtent;
    if (g_extent.width == 0xFFFFFFFF) {
        g_extent.width = ANativeWindow_getWidth(g_window);
        g_extent.height = ANativeWindow_getHeight(g_window);
    }
    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;

    /* Use IDENTITY preTransform when the surface supports it. Setting preTransform =
     * currentTransform tells the presentation engine our content is ALREADY pre-rotated by
     * that amount — but our blit doesn't rotate, so on a device whose surface reports a 90°
     * currentTransform the display then rotates our upright frame 90° (game shows sideways).
     * IDENTITY = "don't rotate what I present", which is what we want. */
    VkSurfaceTransformFlagBitsKHR pretrans =
        (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;

    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_surface,
        .minImageCount = want, .imageFormat = chosen.format, .imageColorSpace = chosen.colorSpace,
        .imageExtent = g_extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = pretrans,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    VkResult cr = g_vk.CreateSwapchainKHR(g_dev, &sci, NULL, &g_swapchain);
    if (cr != VK_SUCCESS) {
        g_swapchain = VK_NULL_HANDLE;
        SWLOGE("present: vkCreateSwapchainKHR failed (%d)", (int)cr);
        return -1;
    }
    g_vk.GetSwapchainImagesKHR(g_dev, g_swapchain, &g_nimg, NULL);
    g_images = calloc(g_nimg, sizeof(VkImage));
    g_vk.GetSwapchainImagesKHR(g_dev, g_swapchain, &g_nimg, g_images);

    banner_log("gpu", "screen output %ux%u, %u buffers, vsync", g_extent.width, g_extent.height, g_nimg);
    return 0;
}

static int memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memprops.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

const char *vkp_modifier_name(uint64_t modifier) {
    static char other[40];
    if (modifier == VKP_MOD_LINEAR) return "linear";
    if (modifier == VKP_MOD_QCOM_COMPRESSED) return "qcom_compressed";
    if (modifier == VKP_MOD_INVALID) return "implicit";
    snprintf(other, sizeof(other), "modifier %#llx", (unsigned long long)modifier);
    return other;
}

/* Can the driver create the image vkp_image_import_dmabuf() creates for this format+modifier
 * (2D, DRM_FORMAT_MODIFIER tiling, TRANSFER_SRC, dma-buf memory) — and import it? */
static int modifier_importable(VkFormat fmt, uint64_t modifier) {
    if (!g_vk.GetPhysicalDeviceImageFormatProperties2) return 1; /* can't ask; the create will tell */
    VkPhysicalDeviceExternalImageFormatInfo ext = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, .pNext = &ext,
        .drmFormatModifier = modifier, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &mod,
        .format = fmt, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkExternalImageFormatProperties extp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &extp};
    if (g_vk.GetPhysicalDeviceImageFormatProperties2(g_pd, &info, &props) != VK_SUCCESS) return 0;
    return (extp.externalMemoryProperties.externalMemoryFeatures &
            VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
}

int vkp_dmabuf_modifiers(uint32_t drm_format, uint64_t *out, int max) {
    if (max <= 0 || dev_init() != 0 || !g_vk.GetPhysicalDeviceFormatProperties2) return 0;
    VkFormat fmt = drm_to_vk(drm_format);
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list};
    g_vk.GetPhysicalDeviceFormatProperties2(g_pd, fmt, &fp);
    if (!list.drmFormatModifierCount) return 0;
    VkDrmFormatModifierPropertiesEXT *props = calloc(list.drmFormatModifierCount, sizeof(*props));
    if (!props) return 0;
    list.pDrmFormatModifierProperties = props;
    g_vk.GetPhysicalDeviceFormatProperties2(g_pd, fmt, &fp);

    int n = 0;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
        uint64_t m = props[i].drmFormatModifier;
        const char *why = NULL;
        if (m != VKP_MOD_LINEAR && m != VKP_MOD_QCOM_COMPRESSED) why = "unknown layout";
        else if (props[i].drmFormatModifierPlaneCount != 1) why = "not single-plane";
        else if (!(props[i].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) why = "no blit source";
        else if (!modifier_importable(fmt, m)) why = "not importable as a dma-buf";
        if (why) {
            LOGI("dmabuf: %c%c%c%c %s reported by the driver but not advertised: %s",
                 drm_format & 0xff, (drm_format >> 8) & 0xff, (drm_format >> 16) & 0xff,
                 (drm_format >> 24) & 0xff, vkp_modifier_name(m), why);
            continue;
        }
        if (n < max) out[n++] = m;
    }
    free(props);
    return n;
}

struct vkp_image *vkp_image_from_dmabuf(int fd, uint32_t drm_format, uint64_t modifier, int w, int h,
                                        uint32_t stride, uint32_t offset) {
    return vkp_image_import_dmabuf(fd, drm_format, modifier, w, h, stride, offset, 0);
}

int vkp_image_is_dmabuf(const struct vkp_image *img) { return img && img->dmabuf && !img->blit_dst; }

struct vkp_image *vkp_image_import_dmabuf(int fd, uint32_t drm_format, uint64_t modifier, int w, int h,
                                          uint32_t stride, uint32_t offset, int as_blit_dst) {
    if (modifier == MOD_INVALID || w <= 0 || h <= 0) return NULL;
    if (dev_init() != 0) return NULL;

    struct vkp_image *img = calloc(1, sizeof(*img));
    if (!img) return NULL;
    img->w = w; img->h = h; img->dmabuf = 1; img->blit_dst = as_blit_dst ? 1 : 0;

    VkSubresourceLayout plane = {.offset = offset, .rowPitch = stride};
    VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = modifier, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane};
    VkExternalMemoryImageCreateInfo extImg = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &modInfo,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &extImg,
        .imageType = VK_IMAGE_TYPE_2D, .format = drm_to_vk(drm_format), .extent = {w, h, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = as_blit_dst ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult cr = g_vk.CreateImage(g_dev, &ici, NULL, &img->image);
    if (cr != VK_SUCCESS) {
        LOGE("%s: vkCreateImage(%s, %dx%d, pitch %u, offset %u) -> %s%s", as_blit_dst ? "layer" : "dmabuf",
             vkp_modifier_name(modifier), w, h, stride, offset, vk_result_name(cr),
             (!as_blit_dst && modifier == VKP_MOD_QCOM_COMPRESSED)
                 ? ": the game's UBWC layout was refused; BANNER_WAYLAND_UBWC=0 forces linear buffers" : "");
        free(img); return NULL;
    }

    int dupfd = dup(fd);
    uint32_t allowed = 0xffffffff;
    if (g_vk.GetMemoryFdPropertiesKHR) {
        VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        if (g_vk.GetMemoryFdPropertiesKHR(g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                          dupfd, &fp) == VK_SUCCESS)
            allowed = fp.memoryTypeBits;
    }
    VkMemoryRequirements req;
    g_vk.GetImageMemoryRequirements(g_dev, img->image, &req);
    uint32_t bits = req.memoryTypeBits & allowed;
    int idx = -1;
    for (int i = 0; i < 32; i++) if (bits & (1u << i)) { idx = i; break; }
    if (idx < 0) {
        LOGE("%s: no memory type can import the %s dma-buf (image types %#x, fd types %#x)",
             as_blit_dst ? "layer" : "dmabuf", vkp_modifier_name(modifier), req.memoryTypeBits, allowed);
        g_vk.DestroyImage(g_dev, img->image, NULL); close(dupfd); free(img); return NULL;
    }

    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                   .fd = dupfd};
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                         .pNext = &imp, .image = img->image};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded,
                                .allocationSize = req.size, .memoryTypeIndex = (uint32_t)idx};
    VkResult mr = g_vk.AllocateMemory(g_dev, &mai, NULL, &img->mem);
    if (mr != VK_SUCCESS) {
        LOGE("%s: importing the %s dma-buf (%dx%d, %llu bytes) -> %s", as_blit_dst ? "layer" : "dmabuf",
             vkp_modifier_name(modifier), w, h, (unsigned long long)req.size, vk_result_name(mr));
        g_vk.DestroyImage(g_dev, img->image, NULL); close(dupfd); free(img); return NULL;
    }
    VkResult br = g_vk.BindImageMemory(g_dev, img->image, img->mem, 0);
    if (br != VK_SUCCESS) {
        LOGE("%s: vkBindImageMemory(%s dma-buf) -> %s", as_blit_dst ? "layer" : "dmabuf",
             vkp_modifier_name(modifier), vk_result_name(br));
        g_vk.FreeMemory(g_dev, img->mem, NULL); g_vk.DestroyImage(g_dev, img->image, NULL);
        free(img); return NULL;
    }
    return img;
}

struct vkp_image *vkp_image_create_shm(int w, int h) {
    if (w <= 0 || h <= 0 || dev_init() != 0) return NULL;

    struct vkp_image *img = calloc(1, sizeof(*img));
    if (!img) return NULL;
    img->w = w; img->h = h;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED};
    if (g_vk.CreateImage(g_dev, &ici, NULL, &img->image) != VK_SUCCESS) { free(img); return NULL; }

    VkMemoryRequirements req;
    g_vk.GetImageMemoryRequirements(g_dev, img->image, &req);
    int idx = memory_type(req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size, .memoryTypeIndex = (uint32_t)idx};
    if (idx < 0 || g_vk.AllocateMemory(g_dev, &mai, NULL, &img->mem) != VK_SUCCESS) {
        g_vk.DestroyImage(g_dev, img->image, NULL); free(img); return NULL;
    }
    g_vk.BindImageMemory(g_dev, img->image, img->mem, 0);

    VkImageSubresource subr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout lay;
    g_vk.GetImageSubresourceLayout(g_dev, img->image, &subr, &lay);
    img->offset = lay.offset;
    img->row_pitch = lay.rowPitch;
    if (g_vk.MapMemory(g_dev, img->mem, 0, req.size, 0, &img->map) != VK_SUCCESS) {
        vkp_image_destroy(img); return NULL;
    }
    return img;
}

/* Renders are synchronous (we wait for the frame's fence), so writing between frames
 * never races the GPU. */
void vkp_image_upload_shm(struct vkp_image *img, const void *data, int stride) {
    if (!img || !img->map || !data) return;
    size_t rowbytes = (size_t)img->w * 4;
    if ((size_t)stride < rowbytes) rowbytes = (size_t)stride;
    for (int y = 0; y < img->h; y++)
        memcpy((uint8_t *)img->map + img->offset + (size_t)y * img->row_pitch,
               (const uint8_t *)data + (size_t)y * stride, rowbytes);
}

int vkp_image_width(const struct vkp_image *img) { return img ? img->w : 0; }
int vkp_image_height(const struct vkp_image *img) { return img ? img->h : 0; }

void vkp_image_destroy(struct vkp_image *img) {
    if (!img) return;
    if (g_dev) {
        if (img->map) g_vk.UnmapMemory(g_dev, img->mem);
        if (img->image) g_vk.DestroyImage(g_dev, img->image, NULL);
        if (img->mem) g_vk.FreeMemory(g_dev, img->mem, NULL);
    }
    free(img);
}

/* Map a draw into swapchain pixels through the scale mode, clipping the destination to the
 * picture's region (the whole output, or its half on TOP/BOTTOM; FILL's overflow is cut here)
 * and trimming the source to match. Returns 0 if nothing is left to draw. */
static int draw_to_blit(const struct vkp_draw *d, VkImageBlit *blit) {
    float kx = g_map.kx, ky = g_map.ky;
    float x0 = g_map.off_x + d->dx * kx, y0 = g_map.off_y + d->dy * ky;
    float x1 = g_map.off_x + (d->dx + d->dw) * kx, y1 = g_map.off_y + (d->dy + d->dh) * ky;
    float sx0 = d->sx, sy0 = d->sy, sx1 = d->sx + d->sw, sy1 = d->sy + d->sh;
    float L = (float)g_map.rx, T = (float)g_map.ry;
    float R = (float)(g_map.rx + g_map.rw), B = (float)(g_map.ry + g_map.rh);
    if (R > (float)g_extent.width) R = (float)g_extent.width;
    if (B > (float)g_extent.height) B = (float)g_extent.height;

    if (x1 <= x0 || y1 <= y0 || sx1 <= sx0 || sy1 <= sy0) return 0;
    if (x0 < L) { sx0 += (L - x0) / (x1 - x0) * (sx1 - sx0); x0 = L; }
    if (y0 < T) { sy0 += (T - y0) / (y1 - y0) * (sy1 - sy0); y0 = T; }
    if (x1 > R) { sx1 -= (x1 - R) / (x1 - x0) * (sx1 - sx0); x1 = R; }
    if (y1 > B) { sy1 -= (y1 - B) / (y1 - y0) * (sy1 - sy0); y1 = B; }

    int ix0 = (int)(x0 + 0.5f), iy0 = (int)(y0 + 0.5f), ix1 = (int)(x1 + 0.5f), iy1 = (int)(y1 + 0.5f);
    int isx0 = (int)sx0, isy0 = (int)sy0, isx1 = (int)(sx1 + 0.5f), isy1 = (int)(sy1 + 0.5f);
    if (isx0 < 0) isx0 = 0;
    if (isy0 < 0) isy0 = 0;
    if (isx1 > d->img->w) isx1 = d->img->w;
    if (isy1 > d->img->h) isy1 = d->img->h;
    if (ix1 <= ix0 || iy1 <= iy0 || isx1 <= isx0 || isy1 <= isy0) return 0;

    *blit = (VkImageBlit){.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .srcOffsets = {{isx0, isy0, 0}, {isx1, isy1, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .dstOffsets = {{ix0, iy0, 0}, {ix1, iy1, 1}}};
    return 1;
}

/* VK_ERROR_DEVICE_LOST: nothing on this device works any more, and there is no way back short
 * of a new session. Say so once and stop touching the swapchain; clients keep being paced by the
 * compositor (see pace_without_output) so they don't wedge, they just aren't shown. */
static void device_lost(const char *where) {
    if (g_dev_state == -2) return;
    g_dev_state = -2;
    banner_log("error", "GPU device lost (VK_ERROR_DEVICE_LOST in %s): the compositor has stopped presenting; "
               "restart the session", where);
    if (g_swapchain) g_vk.DestroySwapchainKHR(g_dev, g_swapchain, NULL);
    g_swapchain = VK_NULL_HANDLE;
    if (g_surface) g_vk.DestroySurfaceKHR(g_inst, g_surface, NULL);
    g_surface = VK_NULL_HANDLE;
    free(g_images);
    g_images = NULL;
    g_nimg = 0;
}

static const char *vk_result_name(VkResult r) {
    switch (r) {
    case VK_ERROR_OUT_OF_DATE_KHR: return "OUT_OF_DATE";
    case VK_SUBOPTIMAL_KHR: return "SUBOPTIMAL";
    case VK_ERROR_SURFACE_LOST_KHR: return "SURFACE_LOST";
    case VK_ERROR_DEVICE_LOST: return "DEVICE_LOST";
    case VK_TIMEOUT: return "TIMEOUT";
    case VK_NOT_READY: return "NOT_READY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT: return "INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT";
    default: {
        static char other[24];
        snprintf(other, sizeof(other), "error %d", (int)r);
        return other;
    }
    }
}

int vkp_render(int scene_w, int scene_h, const struct vkp_draw *draws, int n) {
    vkp_apply_window_request();
    if (g_dev_state == -2) return -1;
    if (dev_init() != 0 || !g_window || scene_w <= 0 || scene_h <= 0) return -1;

    /* Acquire, recreating the swapchain when the surface changed under us (OUT_OF_DATE after a
     * resize/rotation, SURFACE_LOST when the window was torn down) — once, so a frame is not lost
     * to a recreate that would have succeeded. */
    uint32_t img = 0;
    VkResult ar;
    int attempt;
    for (attempt = 0; ; attempt++) {
        if (!g_swapchain && swap_init() != 0) return -1;
        /* A bounded wait: a surface that went away without telling us must not park the
         * compositor thread forever (clients are paced from this thread). */
        ar = g_vk.AcquireNextImageKHR(g_dev, g_swapchain, 1000000000ULL, g_acq, VK_NULL_HANDLE, &img);
        if (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) break;
        if (ar == VK_ERROR_DEVICE_LOST) { device_lost("acquire"); return -1; }
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_ERROR_SURFACE_LOST_KHR) {
            banner_log("gpu", "screen surface %s on acquire: rebuilding the swapchain", vk_result_name(ar));
            destroy_swapchain();
            if (attempt == 0) continue;
            return -1;
        }
        banner_log("error", "present: acquire failed (%s %d)", vk_result_name(ar), (int)ar);
        if (ar == VK_TIMEOUT || ar == VK_NOT_READY) return -1;
        destroy_swapchain(); /* anything else: start over next frame */
        return -1;
    }
    update_map(scene_w, scene_h);

    g_vk.ResetCommandBuffer(g_cmd, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    g_vk.BeginCommandBuffer(g_cmd, &bi);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    /* Source images: take dmabufs from the client's queue family, move shm images to
     * GENERAL once (host writes stay visible across frames: memory is coherent and each
     * frame is a new submission). One barrier per distinct image. */
    VkImageMemoryBarrier *bars = calloc((size_t)n + 1, sizeof(*bars));
    int nb = 0;
    bars[nb++] = (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = g_images[img], .subresourceRange = range, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    for (int i = 0; i < n; i++) {
        struct vkp_image *im = draws[i].img;
        int seen = 0;
        for (int j = 0; j < i; j++) if (draws[j].img == im) { seen = 1; break; }
        if (seen || !im) continue;
        if (im->dmabuf) {
            bars[nb++] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .dstQueueFamilyIndex = g_qfam,
                .image = im->image, .subresourceRange = range, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
        } else if (!im->in_general) {
            bars[nb++] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = im->image, .subresourceRange = range,
                .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
            im->in_general = 1;
        }
    }
    g_vk.CmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, (uint32_t)nb, bars);
    free(bars);

    VkClearColorValue black = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
    g_vk.CmdClearColorImage(g_cmd, g_images[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    {
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
        g_vk.CmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                0, 1, &mb, 0, NULL, 0, NULL);
    }

    int drawn = 0;
    for (int i = 0; i < n; i++) {
        VkImageBlit blit;
        if (!draws[i].img || !draw_to_blit(&draws[i], &blit)) continue;
        g_vk.CmdBlitImage(g_cmd, draws[i].img->image,
                          draws[i].img->dmabuf ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                          g_images[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        drawn++;
    }

    VkImageMemoryBarrier b_present = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_images[img],
        .subresourceRange = range, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    g_vk.CmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            0, 0, NULL, 0, NULL, 1, &b_present);
    g_vk.EndCommandBuffer(g_cmd);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                       .pWaitSemaphores = &g_acq, .pWaitDstStageMask = &wait, .commandBufferCount = 1,
                       .pCommandBuffers = &g_cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_rnd};
    g_vk.ResetFences(g_dev, 1, &g_fence);
    VkResult qr = g_vk.QueueSubmit(g_queue, 1, &si, g_fence);
    if (qr != VK_SUCCESS) {
        if (qr == VK_ERROR_DEVICE_LOST) device_lost("submit");
        else LOGE("present: submit failed (%d)", (int)qr);
        /* The acquired image is never presented; the swapchain would be stuck with it. */
        if (g_dev_state != -2) destroy_swapchain();
        return -1;
    }

    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &g_rnd, .swapchainCount = 1,
                           .pSwapchains = &g_swapchain, .pImageIndices = &img};
    VkResult pr = g_vk.QueuePresentKHR(g_queue, &pi);
    VkResult fr = g_vk.WaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
    if (fr == VK_ERROR_DEVICE_LOST || pr == VK_ERROR_DEVICE_LOST) { device_lost("present"); return -1; }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_ERROR_SURFACE_LOST_KHR) {
        /* The surface changed or went away under this frame: rebuild before the next one. */
        banner_log("gpu", "screen surface %s on present: rebuilding the swapchain", vk_result_name(pr));
        destroy_swapchain();
        return -1;
    } else if (pr == VK_SUBOPTIMAL_KHR || ar == VK_SUBOPTIMAL_KHR) {
        /* Expected and harmless here: the swapchain uses IDENTITY preTransform on purpose (see
         * swap_init), so a rotated panel reports SUBOPTIMAL on every frame. Rebuilding would change
         * nothing (and a rebuild per frame would be far worse), so present as is; say so once. */
        static int said;
        if (!said) { said = 1; banner_log("gpu", "surface reports SUBOPTIMAL (panel rotation); presenting as is"); }
    } else if (pr != VK_SUCCESS) {
        banner_log("error", "present: vkQueuePresentKHR failed (%s %d)", vk_result_name(pr), (int)pr);
        destroy_swapchain();
        return -1;
    }

    /* Signal the app once, on the first real client frame reaching the screen, so the
     * launch/preloader overlay can dismiss (wayland has no XServer window-content hook). */
    if (drawn && !g_first_frame_done) {
        g_first_frame_done = 1;
        banner_on_first_frame();
    }
    return 0;
}

/* ---------------------------------------------------------------- layer mode helpers */

ANativeWindow *vkp_window(void) { return g_window; }

void vkp_signal_first_frame(void) {
    if (g_first_frame_done) return;
    g_first_frame_done = 1;
    banner_on_first_frame();
}

int vkp_update_map(int scene_w, int scene_h) {
    if (g_dev_state == -2 || dev_init() != 0 || !g_window || scene_w <= 0 || scene_h <= 0) return -1;
    if (!g_swapchain && swap_init() != 0) return -1;
    update_map(scene_w, scene_h);
    return g_map.valid ? 0 : -1;
}

int vkp_map_rect(int img_w, int img_h, int scene_w, int scene_h, int out[8]) {
    struct vkp_image tmp = {.w = img_w, .h = img_h}; /* only its size is looked at */
    struct vkp_draw d = {&tmp, 0, 0, (float)img_w, (float)img_h, 0, 0, scene_w, scene_h};
    return vkp_map_draw(&d, out);
}

int vkp_map_draw(const struct vkp_draw *d, int out[8]) {
    VkImageBlit blit;
    if (!d || !d->img || !g_map.valid || !draw_to_blit(d, &blit)) return 0;
    out[0] = blit.srcOffsets[0].x; out[1] = blit.srcOffsets[0].y;
    out[2] = blit.srcOffsets[1].x; out[3] = blit.srcOffsets[1].y;
    out[4] = blit.dstOffsets[0].x; out[5] = blit.dstOffsets[0].y;
    out[6] = blit.dstOffsets[1].x; out[7] = blit.dstOffsets[1].y;
    return 1;
}

/* Copy src (a client frame) into dst (a layer pool buffer) 1:1 and wait for it. Both images are
 * owned by the "foreign" queue family (the game's driver / the display) between our uses, so each
 * use acquires them and the destination is released back for the display to read. */
int vkp_blit_image(struct vkp_image *src, struct vkp_image *dst) {
    if (!src || !dst || !dst->blit_dst || g_dev_state == -2 || dev_init() != 0) return -1;
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    g_vk.ResetCommandBuffer(g_cmd, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    g_vk.BeginCommandBuffer(g_cmd, &bi);
    VkImageMemoryBarrier acq[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = src->dmabuf ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = src->dmabuf ? g_qfam : VK_QUEUE_FAMILY_IGNORED,
         .image = src->image, .subresourceRange = range, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .dstQueueFamilyIndex = g_qfam,
         .image = dst->image, .subresourceRange = range, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT}};
    if (!src->dmabuf) { /* shm image: host-written, GENERAL */
        acq[0].oldLayout = src->in_general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PREINITIALIZED;
        acq[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        acq[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        src->in_general = 1;
    }
    g_vk.CmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, acq);
    int bw = src->w < dst->w ? src->w : dst->w, bh = src->h < dst->h ? src->h : dst->h;
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {bw, bh, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {bw, bh, 1}}};
    g_vk.CmdBlitImage(g_cmd, src->image,
                      src->dmabuf ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                      dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    VkImageMemoryBarrier rel = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = g_qfam,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .image = dst->image, .subresourceRange = range,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    g_vk.CmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            0, 0, NULL, 0, NULL, 1, &rel);
    g_vk.EndCommandBuffer(g_cmd);

    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_cmd};
    g_vk.ResetFences(g_dev, 1, &g_fence);
    VkResult qr = g_vk.QueueSubmit(g_queue, 1, &si, g_fence);
    if (qr != VK_SUCCESS) {
        if (qr == VK_ERROR_DEVICE_LOST) device_lost("layer blit");
        else LOGE("layer: blit submit failed (%d)", (int)qr);
        return -1;
    }
    VkResult fr = g_vk.WaitForFences(g_dev, 1, &g_fence, VK_TRUE, 1000000000ULL);
    if (fr == VK_ERROR_DEVICE_LOST) { device_lost("layer blit"); return -1; }
    if (fr != VK_SUCCESS) { LOGE("layer: blit fence wait -> %s", vk_result_name(fr)); return -1; }
    return 0;
}
