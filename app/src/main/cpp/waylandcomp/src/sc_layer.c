/* Layer mode: the scene on a small set of ASurfaceControl layers — see sc_layer.h and
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
#include <stdatomic.h>
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
    /* Display frame-rate vote for the layer (API 30 / 31). Optional: absent on older Android, where
     * the layer simply carries no vote and the app's surface vote is all there is. */
    void (*setFrameRate)(ASurfaceTransaction *, ASurfaceControl *, float, int8_t);
    void (*setFrameRateStrategy)(ASurfaceTransaction *, ASurfaceControl *, float, int8_t, int8_t);
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
    SYM(setFrameRate, "ASurfaceTransaction_setFrameRate");
    SYM(setFrameRateStrategy, "ASurfaceTransaction_setFrameRateWithChangeStrategy");
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

/* ---- display frame-rate vote (VRR / refresh-rate matching) ------------------------------------ */
/* The app votes a panel cadence with Surface.setFrameRate on the compositor's SurfaceView, but a
 * zero-copy game's frames never touch that surface - they go straight onto the GAME layer - so
 * SurfaceFlinger needs the same vote there or the layer's cadence is invisible to it. The vote
 * belongs to the layer the game is actually presenting on and to no other: the overlay layer
 * carries a window that updates on its own (slow) schedule and is explicitly voted 0, so it never
 * drags the panel. The rate is set from the app thread and applied on the compositor thread with
 * the next transaction, so no transaction is ever created off-thread. 0 = no vote (panel free). */
#define ASC_FRAME_RATE_COMPAT_DEFAULT 0 /* == ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT */
#define ASC_CHANGE_FRAME_RATE_ALWAYS  1 /* == ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS */

static _Atomic float g_fps_want; /* what the app asked for (the game's cadence) */

void sc_layer_set_frame_rate(float fps) {
    atomic_store(&g_fps_want, fps > 0.0f ? fps : 0.0f);
}

/* ---- the layers and their buffer pools -------------------------------------------------------- */

#define POOL_MAX 3

struct slot {
    AHardwareBuffer *ahb;
    struct vkp_image *img;      /* the AHB imported into the compositor's Turnip as a blit target */
    int w, h;
    int release_fd;             /* SurfaceFlinger's release fence: signals when it stopped reading; -1 = none */
    int busy;                   /* set on the layer, or still referenced by SurfaceFlinger */
};

/* One Android display layer: its SurfaceControl, its own buffer pool and its own lifetime. The
 * z-order is fixed by the id (game below, overlay above); nothing is ever re-ordered at runtime. */
struct layer {
    const char *name;
    int32_t z;
    int pool_n;                 /* buffers in this layer's pool (the game cycles more) */
    struct slot slots[POOL_MAX];
    ASurfaceControl *sc;
    ANativeWindow *win;         /* the output window sc was created on */
    int shown;
    int cur_slot;               /* pool slot of the buffer on the layer, -1 = none / not ours */
    void *cur_token;            /* zero-copy: the game's buffer on the layer (NULL = a pool slot) */
    ARect geo_src, geo_dst;
    int geo_valid;
    int linear;                 /* 1: gralloc refused/failed UBWC, this pool is linear */
    uint64_t pool_modifier;
    int first_logged;
    int64_t drop_logged_ns;
    int votes_rate;             /* 1: this layer carries the game's cadence (the game layer) */
    float fps_applied;          /* the vote the live SurfaceControl already carries (-1 = none yet) */
};

static struct layer g_layers[SC_LAYER_COUNT];
static void apply_frame_rate(ASurfaceTransaction *tx, struct layer *l);
static int g_layers_ready;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* pools + callback state */
static int g_pending_cb;                                   /* OnComplete callbacks not yet delivered */
static unsigned g_stat_layer_frames;                             /* frames put on a layer through our own buffers */
static int g_two_logged;
/* A small buffer set on a layer when it is hidden or retired, so SurfaceFlinger replaces (and
 * releases, with a fence) the game's buffer instead of holding it while the layer is invisible. */
static AHardwareBuffer *g_blank;

static void layers_init(void) {
    if (g_layers_ready) return;
    g_layers_ready = 1;
    g_layers[SC_LAYER_GAME] = (struct layer){.name = "banner_wayland_game", .z = 1, .pool_n = 3,
                                             .cur_slot = -1, .votes_rate = 1, .fps_applied = -1.0f};
    g_layers[SC_LAYER_OVERLAY] = (struct layer){.name = "banner_wayland_overlay", .z = 2, .pool_n = 3,
                                                .cur_slot = -1, .votes_rate = 0, .fps_applied = -1.0f};
    for (int i = 0; i < SC_LAYER_COUNT; i++)
        for (int j = 0; j < POOL_MAX; j++) g_layers[i].slots[j].release_fd = -1;
}

static struct layer *layer_of(int id) {
    layers_init();
    return (id >= 0 && id < SC_LAYER_COUNT) ? &g_layers[id] : NULL;
}

struct complete_ctx {
    ASurfaceControl *sc;
    int layer;                  /* which layer's pool prev_slot belongs to */
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
    if (ctx->layer >= 0 && ctx->layer < SC_LAYER_COUNT && ctx->prev_slot >= 0 && ctx->prev_slot < POOL_MAX) {
        struct slot *s = &g_layers[ctx->layer].slots[ctx->prev_slot];
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

static int add_complete(ASurfaceTransaction *tx, struct layer *l, int prev_slot, void *prev_token, int retire) {
    struct complete_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    ctx->sc = l->sc; ctx->layer = (int)(l - g_layers); ctx->prev_slot = prev_slot;
    ctx->prev_token = prev_token; ctx->retire = retire;
    pthread_mutex_lock(&g_lock);
    g_pending_cb++;
    pthread_mutex_unlock(&g_lock);
    api.setOnComplete(tx, ctx, on_complete);
    return 0;
}

/* The stand-in buffer for a hidden/retired layer (allocated once, black, shared by both layers). */
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
static void replace_with_blank(ASurfaceTransaction *tx, struct layer *l) {
    AHardwareBuffer *b = blank_buffer();
    if (b) api.setBuffer(tx, l->sc, b, -1);
}

/* Hide + detach one layer; the SurfaceControl is released from the transaction's callback (the
 * buffer on it stays referenced by SurfaceFlinger until then). */
static void retire_sc(struct layer *l) {
    if (!l->sc) return;
    ASurfaceTransaction *tx = api.txCreate();
    if (tx) {
        replace_with_blank(tx, l);
        api.setVisibility(tx, l->sc, ASC_VISIBILITY_HIDE);
        api.reparent(tx, l->sc, NULL);
        if (add_complete(tx, l, l->cur_slot, l->cur_token, 1) != 0) api.release(l->sc);
        api.txApply(tx);
        api.txDelete(tx);
    } else {
        if (l->cur_token) ahb_swapchain_layer_released(l->cur_token, -1);
        api.release(l->sc);
    }
    banner_log("layer", "%s: SurfaceControl retired (window %p)", l->name, (void *)l->win);
    l->sc = NULL; l->win = NULL;
    l->shown = 0; l->cur_slot = -1; l->cur_token = NULL; l->geo_valid = 0;
    l->fps_applied = -1.0f; /* the next SurfaceControl carries no vote until it is re-applied */
}

/* Wait (bounded) for SurfaceFlinger to finish with every pool buffer of every layer, then free
 * the pools. */
static void drain_and_free_pools(void) {
    int64_t deadline = now_ns() + 300000000LL;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        int pending = g_pending_cb;
        pthread_mutex_unlock(&g_lock);
        if (!pending || now_ns() > deadline) break;
        usleep(2000);
    }
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < SC_LAYER_COUNT; i++) {
        for (int j = 0; j < POOL_MAX; j++) {
            struct slot *s = &g_layers[i].slots[j];
            if (s->release_fd >= 0) { struct pollfd p = {.fd = s->release_fd, .events = POLLIN}; poll(&p, 1, 100); close(s->release_fd); }
            if (s->img) vkp_image_destroy(s->img);
            if (s->ahb) AHardwareBuffer_release(s->ahb);
            memset(s, 0, sizeof(*s));
            s->release_fd = -1;
        }
        g_layers[i].first_logged = 0;
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

static int alloc_slot(struct layer *l, struct slot *s, int w, int h) {
    for (int attempt = 0; attempt < 2; attempt++) {
        AHardwareBuffer_Desc d = {
            .width = (uint32_t)w, .height = (uint32_t)h, .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            /* GPU render target (the blit writes it) + sampled (SurfaceFlinger's GPU fallback reads
             * it). A CPU usage bit makes QTI gralloc allocate linear instead of UBWC. */
            .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     (l->linear ? AHARDWAREBUFFER_USAGE_CPU_READ_RARELY : 0)};
        AHardwareBuffer *ahb = NULL;
        if (AHardwareBuffer_allocate(&d, &ahb) != 0 || !ahb) {
            banner_log("error", "layer: %s: AHardwareBuffer_allocate %dx%d failed", l->name, w, h);
            return -1;
        }
        AHardwareBuffer_Desc got; AHardwareBuffer_describe(ahb, &got);
        const struct banner_native_handle *nh = api.getNativeHandle(ahb);
        uint64_t mod = MOD_LINEAR;
        int known = sniff_modifier(nh, &mod);
        if (!known) {
            /* Not a QTI handle: the only layout we can assume is linear, and only if the buffer was
             * asked for with a CPU bit (gralloc must not have compressed it). */
            if (!l->linear) { AHardwareBuffer_release(ahb); l->linear = 1;
                banner_log("layer", "%s: gralloc handle layout unknown (%d fds, %d ints): using linear pool buffers",
                           l->name, nh ? nh->numFds : -1, nh ? nh->numInts : -1);
                continue; }
            mod = MOD_LINEAR;
        }
        int fd = (nh && nh->numFds > 0) ? nh->data[0] : -1;
        struct vkp_image *img = fd >= 0 ? vkp_image_import_dmabuf(fd, DRM_ABGR8888, mod, w, h, got.stride * 4, 0, 1) : NULL;
        if (!img) {
            banner_log("layer", "%s: import of a %s %dx%d pool buffer (stride %u px) into the compositor's Turnip failed",
                       l->name, mod == MOD_QCOM_COMPRESSED ? "UBWC" : "linear", w, h, got.stride);
            AHardwareBuffer_release(ahb);
            if (!l->linear) { l->linear = 1; continue; } /* retry once with a linear buffer */
            return -1;
        }
        s->ahb = ahb; s->img = img; s->w = w; s->h = h; s->release_fd = -1; s->busy = 0;
        l->pool_modifier = mod;
        banner_log("layer", "%s: pool buffer %dx%d %s, stride %u px (gralloc handle %d fds / %d ints)",
                   l->name, w, h, mod == MOD_QCOM_COMPRESSED ? "UBWC (QCOM_COMPRESSED)" : "linear",
                   got.stride, nh ? nh->numFds : -1, nh ? nh->numInts : -1);
        return 0;
    }
    return -1;
}

/* A slot SurfaceFlinger is done with (waits briefly on its release fence). -1 = none free. */
static int take_free_slot(struct layer *l, int w, int h) {
    int idx = -1, fd = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < l->pool_n; i++) {
        if (i == l->cur_slot || l->slots[i].busy) continue;
        idx = i; fd = l->slots[i].release_fd; l->slots[i].release_fd = -1;
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
    struct slot *s = &l->slots[idx];
    if (s->ahb && (s->w != w || s->h != h)) {
        /* Size change: this slot is free, so it can be replaced at once. */
        vkp_image_destroy(s->img); AHardwareBuffer_release(s->ahb);
        memset(s, 0, sizeof(*s)); s->release_fd = -1;
    }
    if (!s->ahb && alloc_slot(l, s, w, h) != 0) return -1;
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

/* One layer's SurfaceControl on the current output window (created on first use, re-created after
 * a window change). -1 = no window / no API. */
static int ensure_sc(struct layer *l) {
    if (load_api() != 0) return -1;
    ANativeWindow *win = vkp_window();
    if (!win) return -1;
    if (l->sc && l->win != win) retire_sc(l);
    if (!l->sc) {
        l->sc = api.createFromWindow(win, l->name);
        if (!l->sc) { banner_log("error", "layer: ASurfaceControl_createFromWindow(%s) failed", l->name); return -1; }
        l->win = win;
        ASurfaceTransaction *tx = api.txCreate();
        if (tx) {
            api.setZOrder(tx, l->sc, l->z);
            api.setVisibility(tx, l->sc, ASC_VISIBILITY_HIDE);
            apply_frame_rate(tx, l);
            api.txApply(tx); api.txDelete(tx);
        }
        banner_log("layer", "SurfaceControl \"%s\" created as a child of the screen surface (z=%d)", l->name, (int)l->z);
    }
    return 0;
}

/* Geometry of a w x h buffer covering the scene, through the same mapping the blit path and the
 * app's touch mapping use. 1 = r filled, 0 = nothing of it is on screen, -1 = no mapping. */
static int layer_geometry(int w, int h, int scene_w, int scene_h, int r[8]) {
    if (vkp_update_map(scene_w, scene_h) != 0) return -1;
    return vkp_map_rect(w, h, scene_w, scene_h, r) ? 1 : 0;
}

static void apply_geometry(ASurfaceTransaction *tx, struct layer *l, const int r[8]) {
    ARect srcR = {r[0], r[1], r[2], r[3]}, dstR = {r[4], r[5], r[6], r[7]};
    if (!l->geo_valid || memcmp(&srcR, &l->geo_src, sizeof(srcR)) || memcmp(&dstR, &l->geo_dst, sizeof(dstR))) {
        api.setGeometry(tx, l->sc, &srcR, &dstR, 0 /* no transform: the DPU scales, never rotates */);
        l->geo_src = srcR; l->geo_dst = dstR; l->geo_valid = 1;
        banner_log("layer", "%s geometry: buffer %d,%d-%d,%d -> screen %d,%d-%d,%d", l->name,
                   r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    }
}

/* Add the vote to tx when it differs from what this layer carries (compositor thread). Only the
 * layer the game presents on carries the game's rate; every other layer is voted 0, so a slow
 * overlay window can never hold the panel at the game's cadence (or the game's at the overlay's). */
static void apply_frame_rate(ASurfaceTransaction *tx, struct layer *l) {
    float want = l->votes_rate ? atomic_load(&g_fps_want) : 0.0f;
    if (want == l->fps_applied) return;
    if (!api.setFrameRateStrategy && !api.setFrameRate) return;
    if (api.setFrameRateStrategy) {
        /* ALWAYS, like the app's own surface vote: the seamless-only default is ignored by a panel
         * sitting at its peak rate, which is exactly the case we need to move. */
        api.setFrameRateStrategy(tx, l->sc, want, ASC_FRAME_RATE_COMPAT_DEFAULT, ASC_CHANGE_FRAME_RATE_ALWAYS);
    } else {
        api.setFrameRate(tx, l->sc, want, ASC_FRAME_RATE_COMPAT_DEFAULT);
    }
    int first = l->fps_applied < 0.0f;
    l->fps_applied = want;
    if (want > 0.0f) banner_log("layer", "display frame-rate vote on %s: %.2f Hz", l->name, want);
    else if (!first) banner_log("layer", "display frame-rate vote on %s cleared (panel runs free)", l->name);
}

/* Once, when a second layer first goes up: HWC only composes a few layers before SurfaceFlinger
 * falls back to GPU client composition, so the count is deliberately capped and said out loud. */
static void log_layer_count(void) {
    if (g_two_logged) return;
    g_two_logged = 1;
    banner_log("layer", "%d display layers in use: \"%s\" (z=%d) and \"%s\" (z=%d) above it, both children of the "
               "screen surface; the app's own views (drawer, HUD, pointer) stay above both. %d is the cap - more "
               "would push SurfaceFlinger into GPU client composition",
               SC_LAYER_COUNT, g_layers[SC_LAYER_GAME].name, (int)g_layers[SC_LAYER_GAME].z,
               g_layers[SC_LAYER_OVERLAY].name, (int)g_layers[SC_LAYER_OVERLAY].z, SC_LAYER_COUNT);
}

/* The transaction that puts pool slot `idx` of layer `l` on screen at `r`. 0 = applied. */
static int present_slot(struct layer *l, int idx, const int r[8]) {
    struct slot *s = &l->slots[idx];
    ASurfaceTransaction *tx = api.txCreate();
    if (!tx) return -1;
    /* The blit was waited for on the CPU, so no acquire fence is needed (-1). */
    api.setBuffer(tx, l->sc, s->ahb, -1);
    /* The compositor composes with blits, which overwrite: nothing is ever alpha-blended on the
     * copy path either, so every layer is opaque and the picture matches it pixel for pixel. */
    api.setBufferTransparency(tx, l->sc, ASC_TRANSPARENCY_OPAQUE);
    apply_geometry(tx, l, r);
    apply_frame_rate(tx, l);
    if (!l->shown) api.setVisibility(tx, l->sc, ASC_VISIBILITY_SHOW);
    add_complete(tx, l, l->cur_slot, l->cur_token, 0);
    pthread_mutex_lock(&g_lock);
    s->busy = 1;
    pthread_mutex_unlock(&g_lock);
    api.txApply(tx);
    api.txDelete(tx);
    l->cur_slot = idx;
    l->cur_token = NULL;
    if (!l->shown) { l->shown = 1; banner_log("layer", "%s: layer shown", l->name); }
    g_stat_layer_frames++;
    return 0;
}

/* No free buffer this frame: the display still holds all of them. Logged at most every 5 s. */
static void log_drop(struct layer *l) {
    int64_t t = now_ns();
    if (t - l->drop_logged_ns > 5000000000LL) {
        l->drop_logged_ns = t;
        banner_log("layer", "%s: no free layer buffer (display still holds all %d): frame dropped", l->name, l->pool_n);
    }
}

int sc_layer_present_ahb(AHardwareBuffer *ahb, int w, int h, int acquire_fd, void *token, int scene_w, int scene_h) {
    struct layer *l = layer_of(SC_LAYER_GAME);
    int r[8];
    if (!ahb || !token || ensure_sc(l) != 0) goto unavailable;
    int g = layer_geometry(w, h, scene_w, scene_h, r);
    if (g < 0) goto unavailable;
    if (g == 0) { if (acquire_fd >= 0) close(acquire_fd); sc_layer_hide(); return 1; }
    if (token == l->cur_token && l->shown) {
        /* The same frame again (the scene was redrawn for another reason): the display already
         * has it; only the placement may have changed. */
        if (acquire_fd >= 0) close(acquire_fd);
        ASurfaceTransaction *tx = api.txCreate();
        if (tx) { apply_geometry(tx, l, r); apply_frame_rate(tx, l); api.txApply(tx); api.txDelete(tx); }
        return 0;
    }
    ASurfaceTransaction *tx = api.txCreate();
    if (!tx) goto unavailable;
    api.setBuffer(tx, l->sc, ahb, acquire_fd); /* the transaction owns the fence */
    api.setBufferTransparency(tx, l->sc, ASC_TRANSPARENCY_OPAQUE);
    apply_geometry(tx, l, r);
    apply_frame_rate(tx, l);
    if (!l->shown) api.setVisibility(tx, l->sc, ASC_VISIBILITY_SHOW);
    if (add_complete(tx, l, l->cur_slot, l->cur_token == token ? NULL : l->cur_token, 0) != 0) {
        api.txDelete(tx); /* the fence went with the transaction */
        return -1;
    }
    api.txApply(tx);
    api.txDelete(tx);
    l->cur_slot = -1;
    l->cur_token = token;
    if (!l->shown) { l->shown = 1; banner_log("layer", "%s: layer shown", l->name); }
    if (!l->first_logged) { l->first_logged = 1; vkp_signal_first_frame(); }
    return 0;
unavailable:
    if (acquire_fd >= 0) close(acquire_fd);
    return -1;
}

int sc_layer_present(struct vkp_image *src, int scene_w, int scene_h) {
    struct layer *l = layer_of(SC_LAYER_GAME);
    if (!src || ensure_sc(l) != 0) return -1;
    int sw = vkp_image_width(src), sh = vkp_image_height(src);
    int r[8];
    int g = layer_geometry(sw, sh, scene_w, scene_h, r);
    if (g < 0) return -1;
    if (g == 0) { sc_layer_hide(); return 0; } /* nothing of it is on screen */

    int idx = take_free_slot(l, sw, sh);
    if (idx < 0) { log_drop(l); return 0; }
    if (vkp_blit_image(src, l->slots[idx].img) != 0) return -1;
    if (present_slot(l, idx, r) != 0) return -1;
    if (!l->first_logged) {
        l->first_logged = 1;
        banner_log("layer", "presenting %dx%d game frames on their own SurfaceControl layer (%s pool, %d buffers); "
                   "HUD and pointer stay Android views above it", sw, sh,
                   l->pool_modifier == MOD_QCOM_COMPRESSED ? "UBWC" : "linear", l->pool_n);
        vkp_signal_first_frame();
    }
    return 0;
}

int sc_layer_present_pass(const struct vkp_draw *draws, int n, int scene_w, int scene_h) {
    struct layer *l = layer_of(SC_LAYER_GAME);
    int rw = 0, rh = 0, r[8];
    if (!draws || n <= 0 || ensure_sc(l) != 0) return -1;
    if (vkp_update_map(scene_w, scene_h) != 0) return -1;
    /* The chain's result size (a scaling mode resizes to the scene's mapped output size) decides
     * how big the layer buffer has to be, so it is asked for before anything is recorded. */
    if (vkp_pass_target_size(scene_w, scene_h, &rw, &rh) != 0) return -1;
    if (!vkp_map_rect(rw, rh, scene_w, scene_h, r)) { sc_layer_hide(); return 0; }
    int idx = take_free_slot(l, rw, rh);
    if (idx < 0) { log_drop(l); return 0; }
    if (vkp_pass_present_layer(scene_w, scene_h, draws, n, l->slots[idx].img) != 0) return -1;
    if (present_slot(l, idx, r) != 0) return -1;
    if (!l->first_logged) {
        l->first_logged = 1;
        banner_log("layer", "presenting %dx%d frames on their own SurfaceControl layer (%s pool, %d buffers); "
                   "HUD and pointer stay Android views above it", rw, rh,
                   l->pool_modifier == MOD_QCOM_COMPRESSED ? "UBWC" : "linear", l->pool_n);
        vkp_signal_first_frame();
    }
    return 0;
}

int sc_layer_present_overlay(struct vkp_image *src, const int geo[8]) {
    struct layer *l = layer_of(SC_LAYER_OVERLAY);
    if (!src || !geo || ensure_sc(l) != 0) return -1;
    int sw = vkp_image_width(src), sh = vkp_image_height(src);
    if (sw <= 0 || sh <= 0) return -1;
    int idx = take_free_slot(l, sw, sh);
    if (idx < 0) { log_drop(l); return 0; }
    /* The window is copied into the layer buffer 1:1; `geo` crops it and places it, so the layer
     * is exactly the window's rectangle on screen and nothing else is blended anywhere. */
    if (vkp_blit_image(src, l->slots[idx].img) != 0) return -1;
    if (present_slot(l, idx, geo) != 0) return -1;
    if (!l->first_logged) {
        l->first_logged = 1;
        log_layer_count();
    }
    return 0;
}

/* Hide one layer: the buffer comes off with it (a hidden layer would keep the game's buffer - or
 * the pool slot - referenced, and the game needs it back to keep presenting the other way). */
static void hide_layer(struct layer *l) {
    if (!l->sc || !l->shown || api.state != 1) return;
    ASurfaceTransaction *tx = api.txCreate();
    if (!tx) return;
    replace_with_blank(tx, l);
    api.setVisibility(tx, l->sc, ASC_VISIBILITY_HIDE);
    add_complete(tx, l, l->cur_slot, l->cur_token, 0);
    api.txApply(tx); api.txDelete(tx);
    l->shown = 0; l->cur_slot = -1; l->cur_token = NULL;
}

void sc_layer_hide(void) {
    layers_init();
    int said = 0;
    /* Top down, so nothing of the scene is ever uncovered for a frame. */
    for (int i = SC_LAYER_COUNT - 1; i >= 0; i--) {
        if (!g_layers[i].shown) continue;
        hide_layer(&g_layers[i]);
        said = 1;
    }
    if (said) banner_log("layer", "layers hidden (scene is not a single fullscreen window)");
}

void sc_layer_hide_overlay(void) {
    layers_init();
    struct layer *l = &g_layers[SC_LAYER_OVERLAY];
    if (!l->sc) return;
    /* RETIRED, not just hidden. Measured on the Pocket FIT: while a second SurfaceControl exists on
     * the screen surface, SurfaceFlinger keeps composing the whole frame on the GPU
     * (composition: DEVICE/CLIENT) - and hiding the layer does NOT bring it back; only letting the
     * SurfaceControl go does. So the overlay layer lives exactly as long as the window above the
     * game, and the game gets its hardware composition back the moment that window closes. The
     * pool buffers stay allocated for the next one. */
    if (l->shown) hide_layer(l);
    retire_sc(l);
    banner_log("layer", "%s: gone (nothing is above the game any more)", l->name);
}

void sc_layer_window_gone(void) {
    if (api.state != 1) return;
    layers_init();
    for (int i = SC_LAYER_COUNT - 1; i >= 0; i--) retire_sc(&g_layers[i]);
    drain_and_free_pools();
}

unsigned sc_layer_frames_take(void) {
    unsigned n = g_stat_layer_frames;
    g_stat_layer_frames = 0;
    return n;
}
