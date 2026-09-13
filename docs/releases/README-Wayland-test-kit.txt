Bannerlator Wayland test kit  (2026-09-13, phase 2)
===================================================

What is in this folder (only the latest of each kind is kept here):

1. Bannerlator-wayland-phase2-pubg.apk
   App build from branch feat/wayland-phase2 (commit 2aeda707), "pubg" flavour
   (package com.tencent.ig). Install over the existing one; data is kept.
   New since the merged main build:
   - Wayland game driver picker (container, shortcut and XMB settings, under "Compositor driver"):
     Auto (by GPU) / Bundled (Adreno 6xx, 730-750) / Bundled a7xx (710/720/722) /
     Bundled a8xx (830/840, WinNative Balanced) / Bundled a8xx Performance (WinNative PWR_MAX) /
     any imported Linux ICD. Auto picks Balanced on an 8xx. Imports: Contents screen >
     "Wayland game drivers (Linux ICD)" (a zip with a Wayland-built libvulkan_freedreno*.so;
     Android Turnip zips are rejected there on purpose).
   - Pointer lock / mouse-look: games that grab the mouse now get relative motion
     (Half-Life 2 plays with touch mouse-look). Relative Mouse chip and Mouse Warp Override
     are live on Wayland.
   - KEYBOARD WORKS on Wayland (it never did before): typing, shortcuts, WASD.
   - Clipboard both ways (Android <-> Windows programs) and the soft keyboard opens on its own
     when a Windows text field takes focus (tap into the field first).
   - Fullscreen Mode (Off/Fit/Stretch/Fill/Integer) and Screen Alignment (Center/Top/Bottom)
     now work on Wayland, live from the in-game drawer too.
   - Compositor fixes: no black frame / window churn when a game rebuilds its swapchain,
     background + resume recovers the screen, the FPS limiter cap stays exact, HUD sampling
     no longer stalls the compositor thread, swapchain recovery on surface loss.
   - Two crash fixes: a shortcut whose exe has no folder or no ".exe" no longer crashes the launch.
   - EXPERIMENTAL, off by default: put BANNER_WAYLAND_ZERO_COPY=1 in a container's or shortcut's
     Env Vars and a fullscreen game is shown on its own Android hardware layer (SurfaceFlinger
     composes it directly). Proven with Half-Life 2 here; it does not save a copy yet, it is the
     first half of the zero-copy work. Leave it unset for normal play.

2. proton-11.0-2.1-arm64ec-wayland-v3.wcp
   The Proton to use for Wayland containers. Installs as "Proton-11.0-2.1-arm64ec-3"
   (own layer line, never offered as an "update" to a normal 11.0-2 container; if you had the
   -1 or -2 entry, switch your Wayland containers to -3 and remove the old ones on the Contents
   screen).
   Inside: Proton 11.0-2 + winewayland + FOUR Wayland Turnips chosen by the picker above:
     plain      = upstream Mesa 7cda7850, no device patches      (Adreno 6xx, 730, 740, 750)
     a7xx       = Vauzi-17 "710" v3.6 recipe, rebuilt for Wayland (Adreno 710, 720, 722)
     a8xx       = WinNative WN-Turnip 1.15 Balanced recipe        (Adreno 830, 840 + 810/825/829)
     a8xx-perf  = WinNative WN-Turnip 1.15 Performance recipe (holds the GPU at PWR_MAX)
   plus the keyboard fix (winewayland no longer gives up its keyboard when the xkb registry data
   is unreadable). Vauzi recommends TU_DEBUG=sysmem on 710/720/722 for stability (set it in the
   container's Env Vars if you see artifacts).
   It also runs as a normal X11 Proton. OpenGL on Wayland is on (Zink).

3. (No Turnip zip needed.) On Wayland the COMPOSITOR uses whatever Android Turnip you pick under
   "Compositor driver" — any recent one from the in-app catalog works. Do NOT pick "System"
   (the stock driver cannot import the game's frames: black screen).

Setup
-----
1. Install the APK.
2. Make sure an Android Turnip driver is installed (Contents screen > Graphics drivers, any recent one).
3. Install the wcp on the Contents screen (Proton > Install from file).
4. Container settings: Proton = Proton-11.0-2.1-arm64ec-3, Display backend = Wayland,
   Compositor driver = that Android Turnip, Wayland game driver = Auto, FEXCore = an INSTALLED
   version. DXVK / VKD3D versions, Wine components, esync, audio: same as on X11.
5. The FIRST launch after installing the wcp comes up at the wrong size (1024x768): just relaunch.

Verified on this build (AYANEO Pocket FIT, Adreno 750):
  AIO Graphics Test, one launch, Vulkan -> D3D12 -> D3D9 on Wayland, all alive.
  Half-Life 2 (d1_trainstation_01) 144 fps on Wayland with touch mouse-look under a pointer lock.
  Notepad: paste from Android, type, copy back to Android; soft keyboard auto-opens.
  Integer fullscreen mode: picture 1:1 centered, taps land where the arrow is.
  60 fps cap: flat 16.7 ms. HOME and back: picture returns, game keeps running.
  Each of the four bundled game drivers forced on the 750: Wine loads that driver's manifest and
  the test renders (plain / a7xx / a8xx / a8xx Performance).
  (Real Adreno 710/720/8xx hardware NOT tested here: please report.)

Known gaps on Wayland right now
-------------------------------
- Performance headroom: the compositor still copies every frame once. The zero-copy path needs a
  change in the Wayland Turnip's swapchain (next step); the Android-layer half is in (flag above).
- Launching Half-Life 2 closes Wine's desktop process a second after start (fullscreen games are
  unaffected; windowed games launched the same way lose the taskbar). Under investigation.
- Layout names default to "us" on Wayland (the xkb registry data is not bundled yet).
- Drag-and-drop and image clipboard are not bridged (text only).
- Window icons / title-bar decorations are missing (cosmetic).
- Leaving the app pauses the whole container; come back and it resumes. By design.

Logs, if something goes wrong
-----------------------------
- Download/Wayland-logs/wayland-<date>_<time>.log : the compositor's session log
  (windows, frames on screen per 10 s, pointer lock, clipboard, text input, screen mode).
- Download/bannerlator/<game name>/wine_debug.log and the DXVK/VKD3D logs next to it
  (turn on Wine debug logging in Settings > Log Manager).
