#ifndef AHB_SWAPCHAIN_H
#define AHB_SWAPCHAIN_H
/*
 * Zero-copy game frames (layer mode, BANNER_WAYLAND_ZERO_COPY=1): the guest half of the design in
 * ZERO_COPY_SPIKE.md. Our Wayland Turnip (banners-turnip-wayland, patches/wayland/banner_ahb_wsi.py),
 * with BANNER_WSI_AHB=1 and this compositor advertising the private global banner_ahb_v1, allocates
 * each swapchain image as a gralloc AHardwareBuffer, shares its dma-buf through zwp_linux_dmabuf_v1
 * as before (so the blit path still works) and hands us the AHardwareBuffer once per wl_buffer
 * over a socketpair (banner_ahb_v1.attach). When such a buffer is the one fullscreen frame, it goes
 * straight onto the sc_layer SurfaceControl: SurfaceFlinger / the display scan out the game's own
 * buffer, no copy anywhere.
 *
 * Fences stay implicit, in the dma-buf itself: Mesa imports the render fence into the dma-buf
 * before it commits, we export it as the layer's acquire fence (DMA_BUF_IOCTL_EXPORT_SYNC_FILE),
 * and import SurfaceFlinger's release fence back (DMA_BUF_IOCTL_IMPORT_SYNC_FILE) before sending
 * wl_buffer.release, so Mesa's acquire (wsi_create_sync_for_dma_buf_wait) waits for the display.
 *
 * Compositor thread unless noted. Off = the global is not created and nothing here runs.
 */
#include <stddef.h>
#include <stdint.h>
#include <wayland-server.h>

struct dmabuf_buffer;
struct surface;

/* ---- compositor.c -> ahb_swapchain.c */
/* Create the banner_ahb_v1 global (only when layer mode is on) and the release queue. */
void ahb_swapchain_init(struct wl_display *display);
/* 1 if the buffer carries an AHardwareBuffer from the game. */
int ahb_swapchain_has_ahb(const struct dmabuf_buffer *b);
/* Show b (the topmost fullscreen frame, shown by surface s) on the layer without a copy.
 * 0 = done (or the same frame is already up), -1 = unavailable (draw the old way). */
int ahb_swapchain_present(struct dmabuf_buffer *b, struct surface *s, int scene_w, int scene_h);
/* Surface s lets go of buffer b (its wl_buffer `buffer`, NULL if the client destroyed it); paced =
 * the limiter's cadence applies. Returns 1 when the buffer is on the layer: the release is sent
 * from here once SurfaceFlinger's release fence is known. 0 = the caller releases as usual. */
int ahb_swapchain_defer_release(struct dmabuf_buffer *b, struct wl_resource *buffer, struct surface *s, int paced);
/* s is being destroyed: forget it (its deferred releases still go out, unpaced). */
void ahb_swapchain_surface_gone(struct surface *s);
/* Zero-copy frames since the last call (the 10 s summary). */
unsigned ahb_swapchain_stats_take(void);

/* ---- sc_layer.c -> ahb_swapchain.c (SurfaceFlinger's callback thread): the layer let go of the
 * buffer behind `token`; release_fd (owned by the callee, -1 = none) signals when the display is
 * done reading it. */
void ahb_swapchain_layer_released(void *token, int release_fd);

/* ---- ahb_swapchain.c -> compositor.c (hooks) */
struct dmabuf_buffer *banner_dmabuf_from_resource(struct wl_resource *buffer);
int banner_dmabuf_fd(const struct dmabuf_buffer *b);                     /* plane 0's dma-buf */
void banner_dmabuf_size(const struct dmabuf_buffer *b, int *w, int *h);
void **banner_dmabuf_ahb_slot(struct dmabuf_buffer *b);                  /* this module's per-buffer state */
void banner_dmabuf_ref(struct dmabuf_buffer *b);
void banner_dmabuf_unref(struct dmabuf_buffer *b);
/* Give a wl_buffer back to its client now (paced = 0) or on the FPS limiter's cadence (s != NULL). */
void banner_release_buffer(struct surface *s, struct wl_resource *buffer, int paced);
/* "<title>" (program) of the window a surface belongs to, for the log. */
void banner_surface_describe(const struct surface *s, char *out, size_t size);

#endif
