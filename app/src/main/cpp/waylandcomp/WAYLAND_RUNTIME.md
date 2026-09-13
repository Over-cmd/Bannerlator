# Bannerlator Wayland runtime (feat/wayland-runtime)

Experimental **parallel** display runtime: run/launch games through Wine's
`winewayland.drv` talking to our own embedded Wayland compositor, instead of the
X11 path (pure-Java X11 server + `libwinlator.so`). The X11 runtime stays the
default and is untouched — this is a separate flavor/branch.

## What's already proven (spike repo `bannerlator-wayland`, device-tested on Adreno 750)
- Minimal libwayland-server compositor: globals + xdg-shell handshake + buffer commit.
- **Turnip's Vulkan WSI exports real zero-copy dmabufs to our external compositor**
  (same Mesa path winewayland.drv uses for DXVK/VKD3D) — risk #1 retired.
- Compositor imports that dmabuf into its own Turnip VkImage (`vk_import.c`).
The staged `src/` here is that proven code, to grow into the app-embedded compositor.

## Dependencies
1. **A Proton 11 arm64ec wcp that ships `winewayland.drv`** — built on branch
   `The412Banner/proton-wine:feat/winewayland` (task #1). Nothing runs end-to-end
   without it.
2. **Wayland runtime libs in the imagefs** so `winewayland.so` (unixlib) loads:
   `libwayland-client.so`, `libwayland-egl.so`, `libxkbcommon.so`, `libxkbregistry.so`
   (bionic aarch64). The wcp bundles them in its `lib/` as a fallback; the clean home
   is the imagefs — add via `ImageFsInstaller` (new `installWaylandLibs()`), same
   pattern as `installFFmpeg8()`.

## Integration plan (M4)
- **CMake**: add `waylandcomp` as a native lib (`libbannerwayland.so`) built with the
  NDK, linking the bionic `libwayland-server`/`libvulkan`. Generate protocol glue from
  `protocols/*.xml` at build time (host `wayland-scanner`).
- **Surface**: a `WaylandDisplayActivity` (parallel to `XServerDisplayActivity`) hosts a
  `SurfaceView`; JNI hands the `ANativeWindow` to the compositor, which creates a Vulkan
  swapchain on it and blits the imported game VkImage each frame (the last un-proven
  render step; standard Vulkan once the window exists).
- **Input**: Android `MotionEvent`/`KeyEvent` → `wl_seat`/`wl_pointer`/`wl_keyboard`.
- **Launch wiring**: start the compositor, export `WAYLAND_DISPLAY`, select the wayland
  driver per-prefix (registry `Drivers\Graphics = winewayland`) instead of `winex11`,
  and point the container at the winewayland wcp.

## Status
- ✅ **Native lib foundation done + compile-verified.** Compositor + vk_import + pre-generated
  protocol glue build as `libbannerwayland.so` (CMake target added), linking the vendored
  bionic `libwayland-server`. Verified: compiles/links clean as an aarch64 bionic `.so`,
  exports `banner_wayland_run`, NEEDED = libwayland-server + libvulkan. JNI entry
  (`waylandcomp_jni.c`) + `WaylandCompositor.java` bring it up on a thread. Compositor-process
  runtime deps (libwayland-server/libffi/libandroid-support) staged in `jniLibs/arm64-v8a`.
- ⏭️ **Next phase (gated on the winewayland wcp landing green):** `WaylandDisplayActivity`
  (SurfaceView) + JNI `ANativeWindow`→Vulkan swapchain + blit the imported game VkImage to the
  window (last un-proven render step) + input + launch wiring (start compositor, `WAYLAND_DISPLAY`,
  per-prefix `Drivers\Graphics=winewayland`) + `ImageFsInstaller.installWaylandLibs()` (client/egl/xkb
  into the imagefs for winewayland.so).

## Pointer lock / relative mouse (zwp_pointer_constraints_v1 + zwp_relative_pointer_manager_v1)
- Both globals are advertised (version 1); glue is pre-generated in `generated/` with
  `wayland-scanner server-header` / `private-code` from `protocols/*-unstable-v1.xml` (same
  scanner version as the rest, 1.24.0), no build-time scanner needed.
- winewayland's use: `ClipCursor`/fullscreen + hidden cursor → `lock_pointer` (persistent);
  `SetCursorPos` → lock + `set_cursor_position_hint` + commit + unlock; visible cursor +
  `ClipCursor(rect)` → `confine_pointer` with a one-rectangle region. It only turns relative
  motion on for a window its `wl_pointer` has entered, so **pointer focus follows an active
  constraint** (the desktop surface gets `leave`, the constrained surface `enter`) and goes
  back to the desktop on the next motion after the constraint ends.
- Lock: the pointer is frozen; every input delta becomes `relative_motion` (no `motion`).
  Confine: absolute motion is clamped to surface ∩ region. The position hint is applied on
  the surface's commit (pointer moves there) and is where the pointer stays when the lock ends.
  One constraint holds at a time (a newer request ends the older one); oneshot constraints
  are defunct after ending, persistent ones re-take hold on focus re-entry.
- App side: input type 6 = relative delta (1/256 px); `banner_on_pointer_lock(locked, x, y)`
  tells Java to switch its touch/mouse path to deltas (`XServer.setExternalRelativeMode`) and,
  on unlock, to re-sync the X pointer (the absolute input's source) to x,y. With Wayland mode
  `WinHandler.mouseEvent` routes relative-mode input to the compositor instead of the guest.
- Session log tag `pointer`: `lock requested by …`, `locked: … frozen at x,y`, `unlocked: … (why)`,
  `confined: …`, `unconfined: …`, `relative pointer created for …`, `position hint: …`.
  Test FIFO gained `rel DX DY`.

## Screen surface, swapchain recovery (vk_present.c)
- The app's UI thread never waits on the renderer: `vk_present_set_window()` only leaves the new
  `ANativeWindow` (or NULL) in a request slot; the compositor thread applies it before its next
  frame and on every vsync tick (`vkp_apply_window_request`), tearing the old swapchain down and
  releasing the old window's reference. Acquire waits at most 1 s, never forever.
- OUT_OF_DATE / SURFACE_LOST on acquire or present rebuild the swapchain (once inline on acquire, so
  the frame isn't lost). SUBOPTIMAL is presented as is: the swapchain uses IDENTITY preTransform on
  purpose, so a rotated panel reports it on every frame. DEVICE_LOST logs
  `GPU device lost … the compositor has stopped presenting` once and stops touching the device;
  clients keep being paced (`pace_without_output`) so they don't wedge.
- A swapchain that can't be created is retried every 0.5 s and logged once per streak.

## Buffer lifetime, FPS limiter (compositor.c)
- `struct dmabuf_buffer` is reference counted: one ref for the wl_buffer resource, one per surface
  showing it. Mesa destroys a swapchain's wl_buffers when the game rebuilds its swapchain, while the
  last committed one is still on screen; the surface keeps the import (`s->dmabuf_buf`) until its
  next commit, so the window neither blinks to black nor is unmapped/remapped ("closed"/"opened").
- Surface destroy releases its buffer immediately (`drop_dmabuf(s, 0)`), replaced buffers go back on
  the limiter's cadence. The per-surface release schedule is bounded: a slot in the past is brought
  to now, and the schedule never runs further ahead than `(releases still pending + 1)` intervals.

## Fullscreen mode + screen alignment
- `WaylandCompositor.nativeSetScaleMode(fullscreenMode, screenAlignment)` takes the app's
  `Container.FULLSCREEN_OFF/FIT/STRETCH/FILL/INTEGER` (0..4) and `ALIGN_CENTER/TOP/BOTTOM` (0..2),
  any thread, any time (the drawer changes it live). `update_map()` in vk_present.c is a line-for-line
  mirror of `ViewTransformation.update()`: OFF and FIT both letterbox (OFF only differs in the app's
  fullscreen gates), TOP/BOTTOM confine the picture to the top/bottom half of the output (the
  handheld split), FILL overflow is clipped to the region. The app maps touch through the same class
  (TouchpadView → X server → input sink → scene input), the overlay arrow through
  `waylandSceneToView`, and the SurfaceView's own INPUT_SPACE touches through `vkp_output_to_scene`.
- Log tag `screen`: `<mode>, <alignment>: WxH scene shown WxH at x,y on the WxH output` whenever the
  mapping changes (mode, alignment, scene or output size).

## Clipboard, text input, window icons (feat/wayland-clipboard-ime)
Three globals winewayland used to complain about at startup, each in its own file behind the
small interface in `src/banner_ext.h` (compositor.c only gained hook calls + accessors):
- **Clipboard** (`src/wl_clipboard.c`): `wl_data_device_manager` **v3** and
  `zwlr_data_control_manager_v1` **v1**, selection only (start_drag is refused with `cancelled`).
  Wine prefers data-control: its desktop process then owns the clipboard without needing keyboard
  focus and the "clipboard functionality will be limited" ERR goes away too. One shared selection:
  a program's source, or text from Android (`nativeSetClipboardText`), or nothing. Data-control
  devices hear every change; `wl_data_device`s only while their client holds keyboard focus (hook in
  `keyboard_focus()`), and on focus arrival. Guest → Android: a selection with a text mime is read
  once over a pipe on the event loop (1 MiB cap, 3 s timeout) → `banner_on_clipboard_text` →
  `WaylandClipboardSync` → `ClipboardManager`. Android → guest: `OnPrimaryClipChangedListener` plus a
  re-read on resume / window focus (Android hides clipboard changes from background apps); `receive`
  is served by a non-blocking writer. Echo guard on both sides. Log tag `clipboard`.
- **Text input** (`src/wl_text_input.c`): `zwp_text_input_manager_v3` **v1**, double-buffered state,
  `done(serial)` only when we sent events. winewayland enables text input for whichever surface gets
  `enter` and posts IME updates inside *that window's process*, so text input can't follow keyboard
  focus (the desktop surface): it follows the last clicked program window (`g_ime_click`, set in
  `pointer_input`), else the topmost non-shell window. `set_cursor_rectangle` arrives when an edit
  control calls `ImmSetCompositionWindow`; the app (`WaylandTextInput`) shows the soft keyboard only
  then (and with no hardware keyboard), hides it on disable unless the user's own toggle opened it.
  Typed text: `SurfaceInputView`'s `InputConnection` → `commit_string` / `preedit_string` + `done`.
  Deletions become Backspace/Delete key presses — winewayland's `delete_surrounding_text` handler is
  empty. Log tag `text-input`.
- **Window icons** (`src/wl_toplevel_icon.c`): `xdg_toplevel_icon_manager_v1` **v1**, accepts and
  drops icons (`done` at bind, no sizes).
- Host → compositor text crosses as UTF-8 `byte[]` through `banner_ext.c`'s queue (mutex + wake
  pipe), drained on the compositor thread. Glue for the three protocols is pre-generated with
  wayland-scanner 1.24.0 from `protocols/`.

## Compressed (UBWC) game buffers (feat/wayland-ubwc)
- `zwp_linux_dmabuf_v1` used to advertise `LINEAR` (+`INVALID`) only, so Turnip's Wayland WSI in the
  game allocated linear swapchain images and DXVK/VKD3D resolved every frame from their tiled/UBWC
  render targets into that linear copy, before the compositor's own blit. Now `bind_dmabuf` builds
  its table at the first bind from the renderer's driver (`vkp_dmabuf_modifiers`):
  `vkGetPhysicalDeviceFormatProperties2` + `VkDrmFormatModifierPropertiesListEXT` per fourcc
  (AR24/XR24/AB24/XB24), each modifier confirmed with `vkGetPhysicalDeviceImageFormatProperties2`
  for a dma-buf-backed `TRANSFER_SRC` image. Only `DRM_FORMAT_MOD_LINEAR` (0) and
  `DRM_FORMAT_MOD_QCOM_COMPRESSED` (`0x0500000000000001`, UBWC, single memory plane on Adreno) are
  ever advertised; anything else the driver reports goes to logcat as "not advertised".
- Mesa's WSI hands every advertised+supported modifier to `vkCreateImage` as a modifier *list* and
  Turnip picks `QCOM_COMPRESSED` whenever it is in the list (`tu_image.cc`), so with the
  advertisement the game's swapchain is UBWC. The `wl_buffer` then arrives with that modifier and
  the pixel-plane pitch/offset from the game's `vkGetImageSubresourceLayout(MEMORY_PLANE_0)`;
  `vkp_image_import_dmabuf` creates the compositor's image with the explicit modifier and that one
  plane layout (`VkImageDrmFormatModifierExplicitCreateInfoEXT`), which Turnip validates
  (`INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT` if the pitch does not fit its `fdl6` alignment). The
  swapchain blit (`vkp_render`) and the layer-mode blit (`vkp_blit_image`) read it like any other
  source; nothing CPU-side ever touches a dma-buf. Layer mode still blits into its own gralloc pool.
- **A/B switch:** `BANNER_WAYLAND_UBWC=0` (or `false`/`off`) in the container's/shortcut's
  environment variables → `nativeSetUbwc(false)` → linear-only advertisement, exactly the old
  behaviour. Default on. The driver is still queried, so the log shows what it could have done.
- Log tag `dmabuf`: `formats: AR24 linear+qcom_compressed, XR24 …, AB24 …, XB24 …` at the first
  bind (with ` (BANNER_WAYLAND_UBWC=0: qcom_compressed not advertised)` when off, or a second line
  `the compositor's driver (…) reports no importable qcom_compressed layout: game swapchains stay
  linear` when the adrenotools Turnip lacks it). Per window, the existing `vulkan` line now names
  the modifier: `… is presenting GPU frames through Wayland: 1920x1080, format XB24, qcom_compressed
  (zero-copy)` vs `… linear (zero-copy)`. Import failures are `error` lines naming the stage,
  modifier, size, pitch and VkResult (`dmabuf: vkCreateImage(qcom_compressed, …) -> …`).
- Caveat: both sides must agree on the UBWC encoding for the GPU. The game runs the wcp's Wayland
  Turnip, the compositor the user's adrenotools Turnip; the layout code (`fdl6`) is the same, but a
  very different Mesa version on one side is the first suspect if a compressed frame imports fine
  yet looks scrambled — `=0` is the workaround, and the `gpu` line names both the GPU and the
  compositor's driver.

## Zero-copy window layers (spike, `BANNER_WAYLAND_ZERO_COPY=1`)
Research + host-side prototype in `ZERO_COPY_SPIKE.md`: why a dma-buf can't become an
`AHardwareBuffer` (the game's buffers are DMA-heap allocations, not gralloc's), why the interop
must start from a gralloc buffer whose native-handle fd the drivers import (Turnip's own AHB path),
and the WSI patch this needs in our Wayland Turnip. `src/sc_layer.c` implements the receive side:
with the variable in the container's environment, one fullscreen game window is shown on its own
`ASurfaceControl` child of the SurfaceView (pool of three compositor-allocated AHBs, one blit,
release fences from `setOnComplete`, geometry from the fullscreen mode); any other scene falls back
to the swapchain blit. Log tag `layer`. Off by default.

## Zero-copy game frames (feat/wayland-zero-copy-wsi, `BANNER_WAYLAND_ZERO_COPY=1`)
Option (a') of `ZERO_COPY_SPIKE.md`, both halves:
- **Guest (our Wayland Turnip, banners-turnip-wayland `patches/wayland/banner_ahb_wsi.py`, all
  variants):** with `BANNER_WSI_AHB=1` and the compositor advertising `banner_ahb_v1`, the Wayland
  WSI allocates every swapchain image as a gralloc `AHardwareBuffer` (usage GPU_SAMPLED_IMAGE |
  GPU_FRAMEBUFFER | COMPOSER_OVERLAY; UBWC when gralloc picks it and a test `vkCreateImage` with the
  explicit `QCOM_COMPRESSED` layout succeeds, else linear via a CPU usage bit; `BANNER_WSI_AHB_LINEAR=1`
  forces linear), imports the handle's dma-buf with an explicit modifier + pitch (the compositor's own
  pool-import path; `vk_android.c` is not in a platforms=wayland build), shares it through
  `zwp_linux_dmabuf_v1` as before (blit fallback), and sends the `AHardwareBuffer` once per image:
  `socketpair` + `AHardwareBuffer_sendHandleToUnixSocket` + `banner_ahb_v1.attach(wl_buffer, fd, w, h,
  stride, modifier hi/lo, image_count)`. Surface formats are limited to `R8G8B8A8_UNORM/SRGB` (and
  `A2B10G10R10`) while the mode is on: gralloc has no BGRA (DXVK/Zink blit into the swapchain image
  anyway). Marker: `strings libvulkan_freedreno_wayland*.so | grep banner_ahb_v1`.
- **Compositor (`src/ahb_swapchain.c`, protocol `protocols/banner-ahb-v1.xml`):** the global exists only
  in layer mode. `attach` → `AHardwareBuffer_recvHandleFromUnixSocket`, record on the `dmabuf_buffer`.
  When that buffer is the one fullscreen frame (`layer_candidate`, or the layer-only candidate when
  this renderer could not import it), `sc_layer_present_ahb` sets it on the `banner_wayland_game`
  SurfaceControl as is. **Acquire fence** = `DMA_BUF_IOCTL_EXPORT_SYNC_FILE(READ)` of the dma-buf: Mesa
  imports the render fence into the dma-buf (`wsi_signal_dma_buf_from_semaphore`) *before* the
  `wl_surface.commit`, so the export always carries the frame; if the ioctl fails the compositor
  polls the dma-buf instead (logged once). **Release**: `setOnComplete` →
  `getPreviousReleaseFenceFd` → `DMA_BUF_IOCTL_IMPORT_SYNC_FILE(READ)` into the dma-buf → then
  `wl_buffer.release` (still on the FPS limiter's cadence); Mesa's acquire
  (`wsi_create_sync_for_dma_buf_wait`) exports every fence of the dma-buf, so the game waits for the
  display. If the import fails the release waits on the fence fd in the event loop instead. A hidden
  or retired layer gets a 16x16 blank buffer so SurfaceFlinger actually lets go of the game's buffer.
- **App:** `BANNER_WAYLAND_ZERO_COPY=1` (container/shortcut env) also exports `BANNER_WSI_AHB=1` into the
  guest (`XServerDisplayActivity.isWaylandZeroCopyRequested`), one switch for both halves.
- **Log lines (tag `layer`):** `zero-copy: banner_ahb_v1 advertised …`, `zero-copy: <exe> bound
  banner_ahb_v1 …`, `zero-copy: AHB swapchain from <exe> (N images, WxH, UBWC (QCOM_COMPRESSED)|linear,
  stride S px)`, `zero-copy: presenting "<title>" (<exe>) without a copy`; the 10 s `stats` line ends
  with `| N zero-copy frames`. Guest side (Mesa log, stderr of the game): `banner-ahb: WxH swapchain
  (N images) on gralloc buffers: UBWC|linear, stride S px` or the reason it stayed on standard buffers.
