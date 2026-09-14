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
 * only touch the pool under its mutex. Presents run only while g_zero_copy is set (or for a
 * game buffer that can only be shown here); sc_layer_hide() is safe at any time.
 */
#include <stdint.h>
#include <android/hardware_buffer.h>

struct vkp_image;

/* 1 when this device has the SurfaceControl API the layer needs (Android 10+, libnativewindow's
 * getNativeHandle); the reason is logged (tag `layer`) the first time it is missing. */
int sc_layer_available(void);

/* One-shot: does the kernel export a sync_file from the game's dma-buf (DMA_BUF_IOCTL_EXPORT_SYNC_FILE)?
 * Decides where a zero-copy acquire fence can come from; result goes to the session log. */
void sc_layer_probe_dmabuf_fd(int fd);

/* Show `src` (the fullscreen window's imported frame, scene-sized) on the layer, placed through the
 * current fullscreen mode / alignment mapping. 0 = shown (or deliberately dropped: no free buffer),
 * -1 = the layer path is unavailable this frame and the caller must draw the old way. */
int sc_layer_present(struct vkp_image *src, int scene_w, int scene_h);

/* Zero-copy (ahb_swapchain.c): show the game's own w x h AHardwareBuffer on the layer, gated by
 * acquire_fd (a sync_file the layer waits on before reading; owned by the callee, -1 = none).
 * token identifies the buffer: once a later transaction replaces it (or the layer is hidden or
 * retired), ahb_swapchain_layer_released(token, release_fd) reports SurfaceFlinger's release fence
 * for it. Presenting the token already on the layer only updates the placement. 0 = on the layer,
 * 1 = nothing of it is on screen (layer hidden, the buffer was not taken), -1 = unavailable this
 * frame (the caller draws the old way). */
int sc_layer_present_ahb(AHardwareBuffer *ahb, int w, int h, int acquire_fd, void *token,
                         int scene_w, int scene_h);

/* Vote a panel refresh rate for the layer (VRR / refresh-rate matching), the same rate and
 * compatibility the app votes on its own surface with Surface.setFrameRate; 0 = no vote. Needed
 * because a zero-copy game's frames go onto this layer and never reach the app's surface, so the
 * surface vote alone does not describe the game's cadence to SurfaceFlinger. Callable from any
 * thread at any time (it only stores the rate); the compositor applies it on the layer with its
 * next transaction, and re-applies it whenever the SurfaceControl is re-created. A no-op on devices
 * whose libandroid has no ASurfaceTransaction_setFrameRate. */
void sc_layer_set_frame_rate(float fps);

/* The scene is not a single fullscreen window this frame: hide the layer if it is up. */
void sc_layer_hide(void);

/* The output window changed or went away (compositor thread, from vkp_apply_window_request). */
void sc_layer_window_gone(void);

#endif
