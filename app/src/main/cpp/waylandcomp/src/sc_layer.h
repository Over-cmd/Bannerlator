#ifndef SC_LAYER_H
#define SC_LAYER_H
/*
 * Layer mode (BANNER_WAYLAND_ZERO_COPY=1): the scene is handed to SurfaceFlinger as a small,
 * deliberately ordered SET of Android display layers — ASurfaceControl children of the
 * compositor's SurfaceView — instead of being blitted into the compositor's own swapchain.
 *
 *     app window ......... Compose UI, the in-game drawer, the HUD, the on-screen controls and
 *                          the pointer arrow: ordinary Android views, ALWAYS above everything
 *                          below (the SurfaceView is not Z-on-top, so its whole subtree is under
 *                          the window's own content). None of the layers below take input:
 *                          an ASurfaceControl has no input channel, so touch and mouse keep
 *                          reaching the SurfaceView exactly as they did.
 *     +-- SurfaceView ..... the compositor's Vulkan swapchain; black while layer mode is up
 *          +-- z=1 "banner_wayland_game" ...... the one fullscreen game window
 *          +-- z=2 "banner_wayland_overlay" ... at most one window drawn ABOVE the game
 *
 * TWO layers is the hard cap (SC_LAYER_COUNT), and deliberately so: HWC composes only a few
 * layers before SurfaceFlinger falls back to GPU client composition, which would throw away the
 * whole benefit. The count is logged when the second layer first goes up.
 *
 * What can be on the game layer, cheapest first:
 *   - the game's own gralloc buffer (ahb_swapchain.c, true zero-copy: no copy anywhere);
 *   - one blit of the game's frame into a compositor-allocated AHardwareBuffer (sc_layer_present);
 *   - with screen effects on, the compositor pass's result blitted into such a buffer
 *     (sc_layer_present_pass) — the game STAYS on its layer while a Look is applied, and the
 *     scene -> output mapping is done by the display (setGeometry) instead of a second GPU blit.
 * The compositor never alpha-blends (it composes with blits, which overwrite), so every layer is
 * marked OPAQUE and the overlay layer is cropped to the window it carries: the picture is the
 * same as the copy path's, pixel for pixel.
 *
 * Compositor thread only, except the SurfaceFlinger OnComplete callbacks (binder threads), which
 * only touch the pools under g_lock. sc_layer_hide*() is safe at any time.
 */
#include <stdint.h>
#include <android/hardware_buffer.h>

struct vkp_image;
struct vkp_draw;

/* The layers, bottom first. The z-order is the array order (z = id + 1). */
enum sc_layer_id { SC_LAYER_GAME = 0, SC_LAYER_OVERLAY = 1, SC_LAYER_COUNT = 2 };

/* 1 when this device has the SurfaceControl API the layers need (Android 10+, libnativewindow's
 * getNativeHandle); the reason is logged (tag `layer`) the first time it is missing. */
int sc_layer_available(void);

/* One-shot: does the kernel export a sync_file from the game's dma-buf (DMA_BUF_IOCTL_EXPORT_SYNC_FILE)?
 * Decides where a zero-copy acquire fence can come from; result goes to the session log. */
void sc_layer_probe_dmabuf_fd(int fd);

/* GAME layer: show `src` (the fullscreen window's imported frame, scene-sized) on it, placed
 * through the current fullscreen mode / alignment mapping. 0 = shown (or deliberately dropped: no
 * free buffer), -1 = the layer path is unavailable this frame and the caller must draw the old way. */
int sc_layer_present(struct vkp_image *src, int scene_w, int scene_h);

/* GAME layer, screen effects on: run the compositor pass (composite `draws` + the effects chain,
 * vkp_pass_begin) and put its result on the layer. Same return values as sc_layer_present. */
int sc_layer_present_pass(const struct vkp_draw *draws, int n, int scene_w, int scene_h);

/* GAME layer, zero-copy (ahb_swapchain.c): show the game's own w x h AHardwareBuffer, gated by
 * acquire_fd (a sync_file the layer waits on before reading; owned by the callee, -1 = none).
 * token identifies the buffer: once a later transaction replaces it (or the layer is hidden or
 * retired), ahb_swapchain_layer_released(token, release_fd) reports SurfaceFlinger's release fence
 * for it. Presenting the token already on the layer only updates the placement. 0 = on the layer,
 * 1 = nothing of it is on screen (layer hidden, the buffer was not taken), -1 = unavailable this
 * frame (the caller draws the old way). */
int sc_layer_present_ahb(AHardwareBuffer *ahb, int w, int h, int acquire_fd, void *token,
                         int scene_w, int scene_h);

/* Vote a panel refresh rate for the layer the game presents on (VRR / refresh-rate matching), the
 * same rate and compatibility the app votes on its own surface with Surface.setFrameRate; 0 = no
 * vote. Needed because a zero-copy game's frames go onto the GAME layer and never reach the app's
 * surface, so the surface vote alone does not describe the game's cadence to SurfaceFlinger. Only
 * the game layer ever carries it: the overlay layer is explicitly voted 0, so a window that redraws
 * once a second can never hold (or drop) the panel at the game's cadence. Callable from any thread
 * at any time (it only stores the rate); the compositor applies it with the layer's next
 * transaction, and re-applies it whenever a SurfaceControl is re-created. A no-op on devices whose
 * libandroid has no ASurfaceTransaction_setFrameRate. */
void sc_layer_set_frame_rate(float fps);

/* OVERLAY layer: show `src` (one window's imported frame) above the game layer, at the placement
 * `geo` = {src x0,y0,x1,y1 in image pixels, dst x0,y0,x1,y1 in output pixels} from vkp_map_draw.
 * 0 = shown or dropped, -1 = unavailable (the caller must fall back to the copy path). */
int sc_layer_present_overlay(struct vkp_image *src, const int geo[8]);

/* The scene is not a single fullscreen window this frame: hide every layer that is up. */
void sc_layer_hide(void);
/* Only the overlay layer: nothing is above the game any more (the game keeps its layer). */
void sc_layer_hide_overlay(void);

/* The output window changed or went away (compositor thread, from vkp_apply_window_request). */
void sc_layer_window_gone(void);

/* Frames the compositor put on a layer through one of its own buffers (a blit or the effects
 * pass) since the last call — the zero-copy frames ahb_swapchain.c counts are NOT included.
 * For the 10 s summary. */
unsigned sc_layer_frames_take(void);

#endif
