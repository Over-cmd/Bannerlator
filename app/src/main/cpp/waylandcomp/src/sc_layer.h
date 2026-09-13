#ifndef SC_LAYER_H
#define SC_LAYER_H
/*
 * Experimental "layer mode" (BANNER_WAYLAND_ZERO_COPY=1): the one fullscreen game window is
 * shown on its OWN Android layer — an ASurfaceControl child of the compositor's SurfaceView —
 * instead of being blitted into the compositor's swapchain. SurfaceFlinger/HWC then composites
 * the game, the (black) base surface and the app's HUD/cursor views as separate layers.
 *
 * This is the HOST half of the zero-copy design in ZERO_COPY_SPIKE.md: the layer buffer is a
 * compositor-allocated AHardwareBuffer the game frame is blitted into (still one copy, the same
 * cost as today's swapchain blit), so the SurfaceControl path — layer creation, geometry from the
 * fullscreen mode, buffer/fence lifecycle, HWC promotion, vsync pacing — is proven on the device
 * before the guest WSI learns to render straight into such buffers (which removes the copy).
 *
 * Compositor thread only, except the SurfaceFlinger OnComplete callbacks (binder threads), which
 * only touch the pool under its mutex. Never called unless g_zero_copy is set.
 */
#include <stdint.h>

struct vkp_image;

/* One-shot: does the kernel export a sync_file from the game's dma-buf (DMA_BUF_IOCTL_EXPORT_SYNC_FILE)?
 * Decides where a zero-copy acquire fence can come from; result goes to the session log. */
void sc_layer_probe_dmabuf_fd(int fd);

/* Show `src` (the fullscreen window's imported frame, scene-sized) on the layer, placed through the
 * current fullscreen mode / alignment mapping. 0 = shown (or deliberately dropped: no free buffer),
 * -1 = the layer path is unavailable this frame and the caller must draw the old way. */
int sc_layer_present(struct vkp_image *src, int scene_w, int scene_h);

/* The scene is not a single fullscreen window this frame: hide the layer if it is up. */
void sc_layer_hide(void);

/* The output window changed or went away (compositor thread, from vkp_apply_window_request). */
void sc_layer_window_gone(void);

#endif
