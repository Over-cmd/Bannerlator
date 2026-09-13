#ifndef VK_PRESENT_H
#define VK_PRESENT_H
#include <stdint.h>
#include <android/native_window.h>
/*
 * Android-surface render backend for the embedded Wayland compositor.
 * Owns a Turnip VkDevice + a swapchain on the SurfaceView's ANativeWindow. Each
 * client buffer becomes an image (a dmabuf from winewayland's Vulkan WSI is
 * imported zero-copy; a wl_shm buffer is copied into a host-visible image), and
 * every frame blits the whole scene — desktop, windows, subsurfaces — in order.
 */

// Set the Turnip driver to load (adrenotools). Call before the first frame.
// NULL args -> the backend falls back to the system libvulkan (dmabuf import will
// likely fail — Adreno lacks drm_format_modifier). driver_path ends with '/'.
void vk_present_set_driver(const char *driver_path, const char *library_name,
                           const char *native_lib_dir);

// Set/replace the output window (from Surface via ANativeWindow_fromSurface; the backend
// owns the reference from then on). NULL = the surface is gone. Callable from any thread
// and never blocks: the request is applied by the compositor thread (vkp_render /
// vkp_apply_window_request), which tears the old swapchain down and releases the old window.
void vk_present_set_window(ANativeWindow *window);
/* Compositor thread: apply a pending window change now. Returns 1 if the window changed. */
int vkp_apply_window_request(void);

/* How the scene is mapped onto the output (the app's Container.FULLSCREEN_* / ALIGN_* values,
 * mirrored 1:1 from ViewTransformation.java so touch input, which is mapped by the app with
 * that class, lands on the same pixels). Callable from any thread, applies on the next frame. */
enum vkp_scale_mode { VKP_MODE_OFF = 0, VKP_MODE_FIT = 1, VKP_MODE_STRETCH = 2, VKP_MODE_FILL = 3,
                      VKP_MODE_INTEGER = 4 };
enum vkp_align { VKP_ALIGN_CENTER = 0, VKP_ALIGN_TOP = 1, VKP_ALIGN_BOTTOM = 2 };
void vk_present_set_scale_mode(int mode, int alignment);
/* Output pixel (0..output size) -> scene pixel through the current mapping (compositor thread).
 * Returns 0 before the first frame has established a mapping (sx/sy untouched). */
int vkp_output_to_scene(double ox, double oy, double *sx, double *sy);
/* Output size in pixels (0x0 before the first swapchain). */
void vkp_output_size(int *w, int *h);

struct vkp_image;

// Import a dmabuf (single plane). NULL on failure. The image aliases the buffer, so
// later client frames rendered into the same buffer show up without re-importing.
struct vkp_image *vkp_image_from_dmabuf(int fd, uint32_t drm_format, uint64_t modifier,
                                        int w, int h, uint32_t stride, uint32_t offset);
// Same, choosing the role: as_blit_dst = 0 imports a client frame (blit source), 1 imports a
// buffer this backend blits INTO (layer mode's AHardwareBuffer pool, see sc_layer.h).
struct vkp_image *vkp_image_import_dmabuf(int fd, uint32_t drm_format, uint64_t modifier,
                                          int w, int h, uint32_t stride, uint32_t offset,
                                          int as_blit_dst);
int vkp_image_is_dmabuf(const struct vkp_image *img);

// Create a host-visible image and copy BGRA/XRGB8888 pixels into it. NULL on failure.
struct vkp_image *vkp_image_create_shm(int w, int h);
void vkp_image_upload_shm(struct vkp_image *img, const void *data, int stride);

int vkp_image_width(const struct vkp_image *img);
int vkp_image_height(const struct vkp_image *img);
void vkp_image_destroy(struct vkp_image *img);

// One scene draw: the src rectangle of an image (image pixels) scaled into the dst
// rectangle (scene pixels). The scene is mapped onto the output by the scale mode.
struct vkp_draw {
    struct vkp_image *img;
    float sx, sy, sw, sh;
    int dx, dy, dw, dh;
};

// The GPU the renderer runs on ("Adreno (TM) 750"), empty before the device is up.
const char *vkp_gpu_name(void);

// 0 if the renderer can create images (device up), -1 otherwise.
int vkp_ready(void);
/* Whether an output window is attached (or requested); without one vkp_render() draws nothing. */
int vkp_has_window(void);
/* 1 once the Vulkan device was lost: nothing is presented any more (the session must restart). */
int vkp_device_lost(void);

// Clear to black, blit the draws in order (first = bottom) and present.
// Returns 0 on success, -1 if nothing could be presented (no window yet, etc.).
int vkp_render(int scene_w, int scene_h, const struct vkp_draw *draws, int n);

/* ---- layer mode helpers (sc_layer.c; compositor thread) ---- */
/* Whole-image copy of src into dst (a blit-destination dmabuf image), waited for on the CPU. */
int vkp_blit_image(struct vkp_image *src, struct vkp_image *dst);
/* Refresh the scene -> output mapping for this scene size without presenting (creates the
 * swapchain if needed, since the mapping is in output pixels). 0 = mapping valid. */
int vkp_update_map(int scene_w, int scene_h);
/* Map a draw through the current mapping: out = {src x0,y0,x1,y1 (image px), dst x0,y0,x1,y1
 * (output px)}, clipped like the blit path. 0 = nothing of it is visible. */
int vkp_map_draw(const struct vkp_draw *d, int out[8]);
/* The output window frames go to (NULL = none); compositor thread. */
ANativeWindow *vkp_window(void);
/* Fire the one-shot first-frame notification (layer mode presents outside vkp_render). */
void vkp_signal_first_frame(void);

// Session log (compositor.c): one line to Download/Wayland-logs and logcat.
void banner_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
