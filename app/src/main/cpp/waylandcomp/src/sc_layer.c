/* Layer mode: the fullscreen game window on its own ASurfaceControl — see sc_layer.h and
 * ZERO_COPY_SPIKE.md. The ASurfaceControl/ASurfaceTransaction API is dlsym'd from libandroid.so
 * (API 29+), the same way the X11 renderers' scanout code does it, so the library still loads on
 * older devices and the prototype stays off every other path. */
#define _GNU_SOURCE
#include "sc_layer.h"
#include "ahb_swapchain.h"
#include "vk_present.h"
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/rect.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_ABGR8888 FOURCC('A', 'B', '2', '4')   /* R,G,B,A in memory = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM */
#define MOD_LINEAR 0ULL
#define MOD_QCOM_COMPRESSED 0x0500000000000001ULL /* DRM_FORMAT_MOD_QCOM_COMPRESSED (UBWC) */

/* The gralloc buffer handle behind an AHardwareBuffer (AOSP cutils/native_handle.h, which the NDK
 * does not ship; the layout is a stable ABI: fds first, then ints). */
struct banner_native_handle { int version; int numFds; int numInts; int data[]; };

/* ---- libandroid SurfaceControl API (dlsym) ---------------------------------------------------- */

typedef struct ASurfaceControl ASurfaceControl;
typedef struct ASurfaceTransaction ASurfaceTransaction;
typedef struct ASurfaceTransactionStats ASurfaceTransactionStats;
typedef void (*sc_complete_fn)(void *context, ASurfaceTransactionStats *stats);

static struct {
    ASurfaceControl *(*createFromWindow)(ANativeWindow *, const char *);
    void (*release)(ASurfaceControl *);
    ASurfaceTransaction *(*txCreate)(void);
    void (*txDelete)(ASurfaceTransaction *);
    void (*txApply)(ASurfaceTransaction *);
    void (*setBuffer)(ASurfaceTransaction *, ASurfaceControl *, AHardwareBuffer *, int);
    void (*setZOrder)(ASurfaceTransaction *, ASurfaceControl *, int32_t);
    void (*setVisibility)(ASurfaceTransaction *, ASurfaceControl *, int8_t);
    void (*setGeometry)(ASurfaceTransaction *, ASurfaceControl *, const ARect *, const ARect *, int32_t);
    void (*setBufferTransparency)(ASurfaceTransaction *, ASurfaceControl *, int8_t);
    void (*reparent)(ASurfaceTransaction *, ASurfaceControl *, ASurfaceControl *);
    void (*setOnComplete)(ASurfaceTransaction *, void *, sc_complete_fn);
    int (*prevReleaseFence)(ASurfaceTransactionStats *, ASurfaceControl *);
    /* Not in the public NDK headers (vndk/hardware_buffer.h) but exported by libnativewindow.so on
     * every device; Mesa's Android WSI calls it for every gralloc buffer it imports. */
    const void *(*getNativeHandle)(const AHardwareBuffer *);
    int state; /* 0 = untried, 1 = loaded, -1 = unavailable */
} api;

#define ASC_VISIBILITY_HIDE 0
#define ASC_VISIBILITY_SHOW 1
#define ASC_TRANSPARENCY_OPAQUE 2

static int load_api(void) {
    if (api.state) return api.state == 1 ? 0 : -1;
    api.state = -1;
    void *nw = dlopen("libnativewindow.so", RTLD_NOW | RTLD_NOLOAD);
    if (!nw) nw = dlopen("libnativewindow.so", RTLD_NOW);
    api.getNativeHandle = nw ? dlsym(nw, "AHardwareBuffer_getNativeHandle") : NULL;
    if (!api.getNativeHandle) {
        banner_log("layer", "unavailable: libnativewindow.so has no AHardwareBuffer_getNativeHandle");
        return -1;
    }
    void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) { banner_log("layer", "unavailable: dlopen(libandroid.so): %s", dlerror()); return -1; }
#define SYM(field, name) api.field = dlsym(lib, name)
    SYM(createFromWindow, "ASurfaceControl_createFromWindow");
    SYM(release, "ASurfaceControl_release");
    SYM(txCreate, "ASurfaceTransaction_create");
    SYM(txDelete, "ASurfaceTransaction_delete");
    SYM(txApply, "ASurfaceTransaction_apply");
    SYM(setBuffer, "ASurfaceTransaction_setBuffer");
    SYM(setZOrder, "ASurfaceTransaction_setZOrder");
    SYM(setVisibility, "ASurfaceTransaction_setVisibility");
    SYM(setGeometry, "ASurfaceTransaction_setGeometry");
    SYM(setBufferTransparency, "ASurfaceTransaction_setBufferTransparency");
    SYM(reparent, "ASurfaceTransaction_reparent");
    SYM(setOnComplete, "ASurfaceTransaction_setOnComplete");
    SYM(prevReleaseFence, "ASurfaceTransactionStats_getPreviousReleaseFenceFd");
#undef SYM
    if (!api.createFromWindow || !api.release || !api.txCreate || !api.txDelete || !api.txApply ||
        !api.setBuffer || !api.setZOrder || !api.setVisibility || !api.setGeometry ||
        !api.setBufferTransparency || !api.reparent || !api.setOnComplete || !api.prevReleaseFence) {
        banner_log("layer", "unavailable: libandroid.so lacks part of the ASurfaceControl API (Android 10+)");
        return -1;
    }
    api.state = 1;
    return 0;
}

/* ---- buffer pool ------------------------------------------------------------------------------ */

#define POOL 3

struct slot {
    AHardwareBuffer *ahb;
    struct vkp_image *img;      /* the AHB imported into the compositor's Turnip as a blit target */
    int w, h;
    int release_fd;             /* SurfaceFlinger's release fence: signals when it stopped reading; -1 = none */
    int busy;                   /* set on the layer, or still referenced by SurfaceFlinger */
};

static struct slot g_slots[POOL];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* pool + callback state */
static int g_pending_cb;                                   /* OnComplete callbacks not yet delivered */
/* Zero-copy: the game's own buffer on the layer, identified by the token ahb_swapchain.c gave it
 * (NULL = a pool slot, g_cur_slot, is on the layer instead). */
static void *g_cur_token;
/* A small buffer set on the layer when it is hidden or retired, so SurfaceFlinger replaces (and
 * releases, with a fence) the game's buffer instead of holding it while the layer is invisible. */
static AHardwareBuffer *g_blank;

static ASurfaceControl *g_sc;
static ANativeWindow *g_sc_window;                         /* the window g_sc was created on */
static int g_shown;                                        /* layer visible */
static int g_cur_slot = -1;                                /* slot of the buffer on the layer */
static ARect g_geo_src, g_geo_dst;
static int g_geo_valid;
static int g_linear;                                       /* 1: gralloc refused/failed UBWC, pool is linear */
static int g_first_logged;
static int64_t g_drop_logged_ns;
static uint64_t g_pool_modifier;

struct complete_ctx {
    ASurfaceControl *sc;
    int prev_slot;              /* pool buffer this transaction replaced: free once its release fence is known */
    void *prev_token;           /* or the game's buffer it replaced: ahb_swapchain gets its release fence */
    int retire;                 /* release sc after this transaction (hide + reparent) */
};

static int64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Binder thread. */
static void on_complete(void *context, ASurfaceTransactionStats *stats) {
    struct complete_ctx *ctx = context;
    pthread_mutex_lock(&g_lock);
    if (ctx->prev_slot >= 0 && ctx->prev_slot < POOL) {
        struct slot *s = &g_slots[ctx->prev_slot];
        int fd = (stats && ctx->sc) ? api.prevReleaseFence(stats, ctx->sc) : -1;
        if (s->release_fd >= 0) close(s->release_fd);
        s->release_fd = fd;
        s->busy = 0;
    }
    if (g_pending_cb > 0) g_pending_cb--;
    pthread_mutex_unlock(&g_lock);
    if (ctx->prev_token) {
        int fd = (stats && ctx->sc) ? api.prevReleaseFence(stats, ctx->sc) : -1;
        ahb_swapchain_layer_released(ctx->prev_token, fd); /* takes the fd */
    }
    if (ctx->retire && ctx->sc) api.release(ctx->sc);
    free(ctx);
}

static int add_complete(ASurfaceTransaction *tx, ASurfaceControl *sc, int prev_slot, void *prev_token, int retire) {
    struct complete_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    ctx->sc = sc; ctx->prev_slot = prev_slot; ctx->prev_token = prev_token; ctx->retire = retire;
    pthread_mutex_lock(&g_lock);
    g_pending_cb++;
    pthread_mutex_unlock(&g_lock);
    api.setOnComplete(tx, ctx, on_complete);
    return 0;
}

/* The stand-in buffer for a hidden/retired layer (allocated once, black). */
static AHardwareBuffer *blank_buffer(void) {
    if (g_blank) return g_blank;
    AHardwareBuffer_Desc d = {
        .width = 16, .height = 16, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY |
                 AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY};
    if (AHardwareBuffer_allocate(&d, &g_blank) != 0 || !g_blank) { g_blank = NULL; return NULL; }
    void *p = NULL;
    if (AHardwareBuffer_lock(g_blank, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, NULL, &p) == 0 && p) {
        AHardwareBuffer_Desc got; AHardwareBuffer_describe(g_blank, &got);
        memset(p, 0, (size_t)got.stride * 4 * 16);
        AHardwareBuffer_unlock(g_blank, NULL);
    }
    return g_blank;
}

/* Take the current buffer (pool slot or the game's) off the layer by putting the blank one on it,
 * so SurfaceFlinger releases it with a fence through this transaction's callback. */
static void replace_with_blank(ASurfaceTransaction *tx) {
    AHardwareBuffer *b = blank_buffer();
    if (b) api.setBuffer(tx, g_sc, b, -1);
}

/* Hide + detach the layer; the SurfaceControl is released from the transaction's callback (the
 * buffer on it stays referenced by SurfaceFlinger until then). */
static void retire_sc(void) {
    if (!g_sc) return;
    ASurfaceTransaction *tx = api.txCreate();
    if (tx) {
        replace_with_blank(tx);
        api.setVisibility(tx, g_sc, ASC_VISIBILITY_HIDE);
        api.reparent(tx, g_sc, NULL);
        if (add_complete(tx, g_sc, g_cur_slot, g_cur_token, 1) != 0) api.release(g_sc);
        api.txApply(tx);
        api.txDelete(tx);
    } else {
        if (g_cur_token) ahb_swapchain_layer_released(g_cur_token, -1);
        api.release(g_sc);
    }
    banner_log("layer", "SurfaceControl retired (window %p)", (void *)g_sc_window);
    g_sc = NULL; g_sc_window = NULL;
    g_shown = 0; g_cur_slot = -1; g_cur_token = NULL; g_geo_valid = 0;
}

/* Wait (bounded) for SurfaceFlinger to finish with every pool buffer, then free the pool. */
static void drain_and_free_pool(void) {
    int64_t deadline = now_ns() + 300000000LL;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        int pending = g_pending_cb;
        pthread_mutex_unlock(&g_lock);
        if (!pending || now_ns() > deadline) break;
        usleep(2000);
    }
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < POOL; i++) {
        struct slot *s = &g_slots[i];
        if (s->release_fd >= 0) { struct pollfd p = {.fd = s->release_fd, .events = POLLIN}; poll(&p, 1, 100); close(s->release_fd); }
        if (s->img) vkp_image_destroy(s->img);
        if (s->ahb) AHardwareBuffer_release(s->ahb);
        memset(s, 0, sizeof(*s));
        s->release_fd = -1;
    }
    pthread_mutex_unlock(&g_lock);
}

/* Native-handle sniff, the same one Mesa's u_gralloc fallback uses (u_gralloc_fallback.c): a QTI
 * gralloc private_handle_t carries the magic 'gmsm' as its first int and the UBWC flag
 * (PRIV_FLAGS_UBWC_ALIGNED, 0x08000000) in the next one. Returns 0 when the layout is unknown. */
static int sniff_modifier(const struct banner_native_handle *h, uint64_t *mod) {
    const uint32_t gmsm = ('g' << 24) | ('m' << 16) | ('s' << 8) | 'm';
    if (!h || h->numFds < 1 || h->numInts < 2) return 0;
    if ((uint32_t)h->data[h->numFds] != gmsm) return 0;
    *mod = (h->data[h->numFds + 1] & 0x08000000) ? MOD_QCOM_COMPRESSED : MOD_LINEAR;
    return 1;
}

static int alloc_slot(struct slot *s, int w, int h) {
    for (int attempt = 0; attempt < 2; attempt++) {
        AHardwareBuffer_Desc d = {
            .width = (uint32_t)w, .height = (uint32_t)h, .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            /* GPU render target (the blit writes it) + sampled (SurfaceFlinger's GPU fallback reads
             * it). A CPU usage bit makes QTI gralloc allocate linear instead of UBWC. */
            .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     (g_linear ? AHARDWAREBUFFER_USAGE_CPU_READ_RARELY : 0)};
        AHardwareBuffer *ahb = NULL;
        if (AHardwareBuffer_allocate(&d, &ahb) != 0 || !ahb) {
            banner_log("error", "layer: AHardwareBuffer_allocate %dx%d failed", w, h);
            return -1;
        }
        AHardwareBuffer_Desc got; AHardwareBuffer_describe(ahb, &got);
        const struct banner_native_handle *nh = api.getNativeHandle(ahb);
        uint64_t mod = MOD_LINEAR;
        int known = sniff_modifier(nh, &mod);
        if (!known) {
            /* Not a QTI handle: the only layout we can assume is linear, and only if the buffer was
             * asked for with a CPU bit (gralloc must not have compressed it). */
            if (!g_linear) { AHardwareBuffer_release(ahb); g_linear = 1;
                banner_log("layer", "gralloc handle layout unknown (%d fds, %d ints): using linear pool buffers",
                           nh ? nh->numFds : -1, nh ? nh->numInts : -1);
                continue; }
            mod = MOD_LINEAR;
        }
        int fd = (nh && nh->numFds > 0) ? nh->data[0] : -1;
        struct vkp_image *img = fd >= 0 ? vkp_image_import_dmabuf(fd, DRM_ABGR8888, mod, w, h, got.stride * 4, 0, 1) : NULL;
        if (!img) {
            banner_log("layer", "import of a %s %dx%d pool buffer (stride %u px) into the compositor's Turnip failed",
                       mod == MOD_QCOM_COMPRESSED ? "UBWC" : "linear", w, h, got.stride);
            AHardwareBuffer_release(ahb);
            if (!g_linear) { g_linear = 1; continue; } /* retry once with a linear buffer */
            return -1;
        }
        s->ahb = ahb; s->img = img; s->w = w; s->h = h; s->release_fd = -1; s->busy = 0;
        g_pool_modifier = mod;
        banner_log("layer", "pool buffer %dx%d %s, stride %u px (gralloc handle %d fds / %d ints)", w, h,
                   mod == MOD_QCOM_COMPRESSED ? "UBWC (QCOM_COMPRESSED)" : "linear", got.stride,
                   nh ? nh->numFds : -1, nh ? nh->numInts : -1);
        return 0;
    }
    return -1;
}

/* A slot SurfaceFlinger is done with (waits briefly on its release fence). -1 = none free. */
static int take_free_slot(int w, int h) {
    int idx = -1, fd = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < POOL; i++) {
        if (i == g_cur_slot || g_slots[i].busy) continue;
        idx = i; fd = g_slots[i].release_fd; g_slots[i].release_fd = -1;
        break;
    }
    pthread_mutex_unlock(&g_lock);
    if (idx < 0) return -1;
    if (fd >= 0) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int r;
        do { r = poll(&p, 1, 100); } while (r < 0 && errno == EINTR);
        close(fd);
        if (r == 0) return -1; /* still read by the display: leave it, drop this frame */
    }
    struct slot *s = &g_slots[idx];
    if (s->ahb && (s->w != w || s->h != h)) {
        /* Size change: this slot is free, so it can be replaced at once. */
        vkp_image_destroy(s->img); AHardwareBuffer_release(s->ahb);
        memset(s, 0, sizeof(*s)); s->release_fd = -1;
    }
    if (!s->ahb && alloc_slot(s, w, h) != 0) return -1;
    return idx;
}

/* ---- public --------------------------------------------------------------------------------- */

int sc_layer_available(void) { return load_api() == 0; }

void sc_layer_probe_dmabuf_fd(int fd) {
    static int done;
    if (done || fd < 0) return;
    done = 1;
    struct { uint32_t flags; int32_t fd; } exp = {.flags = 1u /* DMA_BUF_SYNC_READ */, .fd = -1};
    /* DMA_BUF_IOCTL_EXPORT_SYNC_FILE = _IOWR('b', 2, struct dma_buf_export_sync_file) */
    if (ioctl(fd, _IOWR('b', 2, exp), &exp) == 0) {
        banner_log("layer", "kernel exports sync_file fences from the game's dma-buf: zero-copy acquire fences can come from the buffer itself");
        if (exp.fd >= 0) close(exp.fd);
    } else {
        banner_log("layer", "DMA_BUF_IOCTL_EXPORT_SYNC_FILE on the game's dma-buf failed (%s): acquire fences must be exported by the game's driver (sync_fd) instead",
                   strerror(errno));
    }
}

/* The SurfaceControl on the current output window (created on first use, re-created after a
 * window change). -1 = no window / no API. */
static int ensure_sc(void) {
    if (load_api() != 0) return -1;
    ANativeWindow *win = vkp_window();
    if (!win) return -1;
    if (g_sc && g_sc_window != win) retire_sc();
    if (!g_sc) {
        g_sc = api.createFromWindow(win, "banner_wayland_game");
        if (!g_sc) { banner_log("error", "layer: ASurfaceControl_createFromWindow failed"); return -1; }
        g_sc_window = win;
        ASurfaceTransaction *tx = api.txCreate();
        if (tx) {
            api.setZOrder(tx, g_sc, 1);
            api.setVisibility(tx, g_sc, ASC_VISIBILITY_HIDE);
            api.txApply(tx); api.txDelete(tx);
        }
        banner_log("layer", "SurfaceControl \"banner_wayland_game\" created as a child of the screen surface");
    }
    return 0;
}

/* Geometry of a w x h buffer covering the scene, through the same mapping the blit path and the
 * app's touch mapping use. 1 = r filled, 0 = nothing of it is on screen, -1 = no mapping. */
static int layer_geometry(int w, int h, int scene_w, int scene_h, int r[8]) {
    if (vkp_update_map(scene_w, scene_h) != 0) return -1;
    return vkp_map_rect(w, h, scene_w, scene_h, r) ? 1 : 0;
}

static void apply_geometry(ASurfaceTransaction *tx, const int r[8]) {
    ARect srcR = {r[0], r[1], r[2], r[3]}, dstR = {r[4], r[5], r[6], r[7]};
    if (!g_geo_valid || memcmp(&srcR, &g_geo_src, sizeof(srcR)) || memcmp(&dstR, &g_geo_dst, sizeof(dstR))) {
        api.setGeometry(tx, g_sc, &srcR, &dstR, 0 /* no transform: the DPU scales, never rotates */);
        g_geo_src = srcR; g_geo_dst = dstR; g_geo_valid = 1;
        banner_log("layer", "geometry: buffer %d,%d-%d,%d -> screen %d,%d-%d,%d", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    }
}

int sc_layer_present_ahb(AHardwareBuffer *ahb, int w, int h, int acquire_fd, void *token, int scene_w, int scene_h) {
    int r[8];
    if (!ahb || !token || ensure_sc() != 0) goto unavailable;
    int g = layer_geometry(w, h, scene_w, scene_h, r);
    if (g < 0) goto unavailable;
    if (g == 0) { if (acquire_fd >= 0) close(acquire_fd); sc_layer_hide(); return 1; }
    if (token == g_cur_token && g_shown) {
        /* The same frame again (the scene was redrawn for another reason): the display already
         * has it; only the placement may have changed. */
        if (acquire_fd >= 0) close(acquire_fd);
        ASurfaceTransaction *tx = api.txCreate();
        if (tx) { apply_geometry(tx, r); api.txApply(tx); api.txDelete(tx); }
        return 0;
    }
    ASurfaceTransaction *tx = api.txCreate();
    if (!tx) goto unavailable;
    api.setBuffer(tx, g_sc, ahb, acquire_fd); /* the transaction owns the fence */
    api.setBufferTransparency(tx, g_sc, ASC_TRANSPARENCY_OPAQUE);
    apply_geometry(tx, r);
    if (!g_shown) api.setVisibility(tx, g_sc, ASC_VISIBILITY_SHOW);
    if (add_complete(tx, g_sc, g_cur_slot, g_cur_token == token ? NULL : g_cur_token, 0) != 0) {
        api.txDelete(tx); /* the fence went with the transaction */
        return -1;
    }
    api.txApply(tx);
    api.txDelete(tx);
    g_cur_slot = -1;
    g_cur_token = token;
    if (!g_shown) { g_shown = 1; banner_log("layer", "layer shown"); }
    if (!g_first_logged) { g_first_logged = 1; vkp_signal_first_frame(); }
    return 0;
unavailable:
    if (acquire_fd >= 0) close(acquire_fd);
    return -1;
}

int sc_layer_present(struct vkp_image *src, int scene_w, int scene_h) {
    if (!src || ensure_sc() != 0) return -1;
    int sw = vkp_image_width(src), sh = vkp_image_height(src);
    int r[8];
    int g = layer_geometry(sw, sh, scene_w, scene_h, r);
    if (g < 0) return -1;
    if (g == 0) { sc_layer_hide(); return 0; } /* nothing of it is on screen */

    int idx = take_free_slot(sw, sh);
    if (idx < 0) {
        int64_t t = now_ns();
        if (t - g_drop_logged_ns > 5000000000LL) {
            g_drop_logged_ns = t;
            banner_log("layer", "no free layer buffer (display still holds all %d): frame dropped", POOL);
        }
        return 0;
    }
    struct slot *s = &g_slots[idx];
    if (vkp_blit_image(src, s->img) != 0) return -1;

    ASurfaceTransaction *tx = api.txCreate();
    if (!tx) return -1;
    /* The blit was waited for on the CPU, so no acquire fence is needed (-1). Next step: export the
     * submit fence as a sync_fd and hand it here instead of waiting. */
    api.setBuffer(tx, g_sc, s->ahb, -1);
    api.setBufferTransparency(tx, g_sc, ASC_TRANSPARENCY_OPAQUE);
    apply_geometry(tx, r);
    if (!g_shown) api.setVisibility(tx, g_sc, ASC_VISIBILITY_SHOW);
    add_complete(tx, g_sc, g_cur_slot, g_cur_token, 0);
    pthread_mutex_lock(&g_lock);
    s->busy = 1;
    pthread_mutex_unlock(&g_lock);
    api.txApply(tx);
    api.txDelete(tx);
    g_cur_slot = idx;
    g_cur_token = NULL;
    if (!g_shown) { g_shown = 1; banner_log("layer", "layer shown"); }
    if (!g_first_logged) {
        g_first_logged = 1;
        banner_log("layer", "presenting %dx%d game frames on their own SurfaceControl layer (%s pool, %d buffers); "
                   "HUD and pointer stay Android views above it", sw, sh,
                   g_pool_modifier == MOD_QCOM_COMPRESSED ? "UBWC" : "linear", POOL);
        vkp_signal_first_frame();
    }
    return 0;
}

void sc_layer_hide(void) {
    if (!g_sc || !g_shown || api.state != 1) return;
    ASurfaceTransaction *tx = api.txCreate();
    if (!tx) return;
    /* The buffer comes off the layer with it: a hidden layer would keep the game's buffer (or the
     * pool slot) referenced, and the game needs it back to keep presenting the other way. */
    replace_with_blank(tx);
    api.setVisibility(tx, g_sc, ASC_VISIBILITY_HIDE);
    add_complete(tx, g_sc, g_cur_slot, g_cur_token, 0);
    api.txApply(tx); api.txDelete(tx);
    g_shown = 0; g_cur_slot = -1; g_cur_token = NULL;
    banner_log("layer", "layer hidden (scene is not a single fullscreen window)");
}

void sc_layer_window_gone(void) {
    if (api.state != 1) return;
    retire_sc();
    drain_and_free_pool();
}
