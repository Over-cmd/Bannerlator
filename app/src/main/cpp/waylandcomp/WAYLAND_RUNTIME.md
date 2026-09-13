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

## Zero-copy window layers (spike, `BANNER_WAYLAND_ZERO_COPY=1`)
Research + host-side prototype in `ZERO_COPY_SPIKE.md`: why a dma-buf can't become an
`AHardwareBuffer` (the game's buffers are DMA-heap allocations, not gralloc's), why the interop
must start from a gralloc buffer whose native-handle fd the drivers import (Turnip's own AHB path),
and the WSI patch this needs in our Wayland Turnip. `src/sc_layer.c` implements the receive side:
with the variable in the container's environment, one fullscreen game window is shown on its own
`ASurfaceControl` child of the SurfaceView (pool of three compositor-allocated AHBs, one blit,
release fences from `setOnComplete`, geometry from the fullscreen mode); any other scene falls back
to the swapchain blit. Log tag `layer`. Off by default.
