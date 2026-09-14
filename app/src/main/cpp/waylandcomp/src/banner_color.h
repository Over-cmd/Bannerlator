#ifndef BANNER_COLOR_H
#define BANNER_COLOR_H
/*
 * HDR10 output on the Wayland backend, round 1 (opt-in) — the compositor half of HDR_RECON.md
 * Phase A, implemented in wl_color_mgmt.c. The game half already ships: Mesa's Wayland WSI in our
 * Turnip is a wp_color_manager_v1 client and exposes VK_COLOR_SPACE_HDR10_ST2084_EXT as soon as a
 * compositor advertises BT.2020 + ST 2084; DXVK takes it with DXVK_HDR=1.
 *
 * THE GATE. Nothing here exists for a session unless all of these hold when the compositor starts:
 *   - BANNER_WAYLAND_HDR=1 in the container's or the shortcut's environment (the opt-in);
 *   - the display the game is on lists HDR10 among its supported HDR types (read by the app from
 *     android.view.Display — a property of the connector, not of the device);
 *   - display layers with dataspace control (ASurfaceTransaction_setBufferDataSpace, Android 10+)
 *     and the zero-copy global (banner_ahb_v1): an HDR frame is only honest on the game's OWN
 *     display layer, because everything the compositor draws itself is 8-bit sRGB.
 * A closed gate advertises nothing — no colour-management global, no 10-bit dma-buf formats — so the
 * session is byte-for-byte what it was before this file existed, and the session log says why.
 * BANNER_WAYLAND_HDR=force skips the display check only (testing the negotiation on an SDR panel;
 * SurfaceFlinger then tone-maps the layer, and says so in its composition type).
 *
 * WHAT AN OPEN GATE OFFERS (exactly the subset Mesa binds, HDR_RECON.md §2.2): wp_color_manager_v1
 * version 1 with the perceptual intent, the parametric creator + mastering-display metadata,
 * BT.2020 primaries and the ST 2084 (PQ) transfer function; zwp_linux_dmabuf_v1 adds AB30/XB30
 * (A2B10G10R10, the only 10-bit layout our zero-copy WSI can put in a gralloc buffer). A surface's
 * image description is double-buffered on wl_surface.commit; its frames go onto the game's display
 * layer tagged BT2020_PQ with the game's SMPTE 2086 / CTA-861.3 metadata (sc_layer.c).
 *
 * Compositor thread unless noted.
 */
#include <stdint.h>
#include <wayland-server.h>

/* ADataSpace values (NDK <android/data_space.h>) the layer path emits. */
#define BANNER_ADATASPACE_UNKNOWN    0
#define BANNER_ADATASPACE_BT2020_PQ  163971072 /* STANDARD_BT2020 | TRANSFER_ST2084 | RANGE_FULL = 0x09c60000 */
#define BANNER_ADATASPACE_BT2020_HLG 168165376 /* STANDARD_BT2020 | TRANSFER_HLG | RANGE_FULL */

/* One image description as the program made it, and what it becomes on a display layer. Immutable:
 * a changed description is a new record with a new identity. */
struct banner_color {
    uint32_t identity;                   /* the id the `ready` event carried (never 0) */
    uint32_t primaries, tf;              /* wp_color_manager_v1 named values */
    int32_t dataspace;                   /* ADataSpace on a display layer; 0 = none (treated as sRGB) */
    int has_st2086;                      /* SMPTE ST 2086: mastering display primaries + luminance */
    float red[2], green[2], blue[2], white[2]; /* CIE 1931 xy */
    float max_lum, min_lum;              /* nits */
    int has_cta861;                      /* CTA-861.3 */
    float max_cll, max_fall;             /* nits; 0 = not given */
    char text[256];                      /* the session log's one-line summary of it */
};

/* ---- app -> compositor (JNI, any thread) */
/* mode 0 = off, 1 = BANNER_WAYLAND_HDR=1, 2 = BANNER_WAYLAND_HDR=force (testing). source names where
 * the switch came from; dxvk_hdr = DXVK_HDR=1 is in the game's environment; zero_copy_forced = the
 * app turned zero-copy presentation on for this session because HDR needs it. Before the start. */
void banner_color_set_request(int mode, const char *source, int dxvk_hdr, int zero_copy_forced);
/* The display the game is on, as android.view.Display reports it. Before the start (it feeds the
 * gate) and again whenever it changes (logged; the gate is decided once per session). */
void banner_color_set_display(int id, const char *name, const char *formats, int hdr10, float max_lum,
                              float max_avg, float min_lum, int ratio_available, float ratio, int api);
/* One reading of Display.getHdrSdrRatio() (API 34+; < 0 = not available). listener = it came from
 * the display's ratio listener rather than the app's periodic sampler. */
void banner_color_ratio_sample(float ratio, int listener);
/* Milliseconds since an HDR frame last went onto a display layer; -1 = none this session. */
int banner_color_last_frame_age_ms(void);
/* -1 = the compositor has not decided yet, 0 = closed, 1 = open. */
int banner_color_gate_state(void);
/* The session is ending: write the summary line ("HDR on screen: …"). */
void banner_color_session_end(void);

/* ---- compositor.c -> here */
/* Decide the gate (call after ahb_swapchain_init) and create the global when it is open. */
void banner_color_init(struct wl_display *display);
/* The gate is open: the 10-bit dma-buf formats may be advertised, descriptions may exist. */
int banner_color_hdr_open(void);
/* wl_surface.commit: the pending image description (if any request touched it) becomes current. */
void banner_color_commit(struct wl_resource *surface);
/* The surface's current image description; NULL = none (sRGB, today's default). */
const struct banner_color *banner_color_of(struct wl_resource *surface);
/* A program disconnected: if it presented HDR, its part of the summary is written now. */
void banner_color_client_gone(struct wl_client *client);
/* The 10 s summary tick (one `color` line when anything HDR happened in the window). */
void banner_color_stats_tick(void);
/* A frame of an HDR-described surface went through the compositor's own 8-bit sRGB pass this scene
 * (who = the window, reason = why it did not get the display layer). Shown untone-mapped. */
void banner_color_frame_copied(const char *who, const char *reason);

/* ---- sc_layer.c -> here */
/* A NEW frame of an HDR-described surface went onto a display layer tagged with c's dataspace.
 * zero_copy = the game's own buffer (else the compositor's 8-bit layer copy); ahb_format = its
 * AHardwareBuffer format. */
void banner_color_frame_on_layer(const struct banner_color *c, int zero_copy, uint32_t ahb_format);
/* "RGBA1010102 (10-bit)" etc. for an AHARDWAREBUFFER_FORMAT_* value (static buffer). */
const char *banner_ahb_format_name(uint32_t format);

#endif
