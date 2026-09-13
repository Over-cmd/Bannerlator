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
