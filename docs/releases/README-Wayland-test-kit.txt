Bannerlator Wayland test kit  (2026-09-13, phase 3 = pre-release 2)
====================================================================

1. Bannerlator-3.1.2-wayland-pre2-<flavour>.apk   (all three flavours on the GitHub pre-release)
   versionCode 85 like 3.1.1, so you can go back to 3.1.1 or forward to the next stable.
   New since pre-release 1:
   - Eight Wayland game drivers built into the Proton (see 2). Auto picks by GPU; the a8xx
     alternatives are for Adreno 830/840 owners to compare.
   - Zero-copy presentation (experimental, off by default): put BANNER_WAYLAND_ZERO_COPY=1 in the
     container's or shortcut's Env Vars and a fullscreen game's own frames go straight to the display
     hardware, no compositor copy (session log shows "N zero-copy frames"). Proven with Half-Life 2.
   - Compressed (UBWC) game buffers are accepted by the compositor (default on;
     BANNER_WAYLAND_UBWC=0 forces linear if you see a scrambled picture on your GPU).
   - Fixes: the first launch after installing a Wayland Proton no longer comes up at 1024x768;
     Wine's desktop no longer closes a second into a game's startup (32-bit games under FEX).
   - Keyboard layout names now come from xkb data bundled in the Proton (no longer forced to "us").

2. proton-11.0-2.1-arm64ec-wayland-v5.wcp   (installs as Proton-11.0-2.1-arm64ec-5)
   Proton 11.0-2 + winewayland + EIGHT Wayland Turnips chosen under "Wayland game driver":
     Bundled                    upstream Mesa 7cda7850, no patches        Adreno 6xx, 730, 740, 750
     Bundled a7xx               Vauzi-17 "710" v3.6 recipe                  Adreno 710, 720, 722
     Bundled a8xx               WinNative WN-Turnip 1.15 Balanced (Auto on 8xx)   Adreno 830/840
     Bundled a8xx Performance   WinNative WN-Turnip 1.15 Performance (PWR_MAX)
     Bundled a8xx gen8          Banners-Turnip gen8 recipe (own Android a8xx job)
     Bundled a8xx SMXZ          StevenMXZ Turnip Gen8 V36 recipe
     Bundled a8xx WHITE         whitebelyash Mainline Turnip v31 recipe
     Bundled a8xx upstream      pure Mesa main @ bbc7792f (2026-09-13), no patches
   Also inside: the zero-copy swapchain patch in every driver, xkeyboard-config data, and the
   Wine fix restoring the 1 s desktop-close grace. Remove older -1 … -4 entries on the Contents screen.
   All eight load and render on an Adreno 750; none has been run on real 710/720/722 or 830/840 yet.

3. No Turnip zip needed: the compositor uses whatever Android Turnip you pick under
   "Compositor driver" (any recent one from the in-app catalog). Never pick "System".

Setup
-----
1. Install the APK for your flavour over 3.1.1 or pre-release 1.
2. Contents > Proton > Install from file: the v5 wcp.
3. Container: Proton = Proton-11.0-2.1-arm64ec-5, Display backend = Wayland, Compositor driver =
   an Android Turnip, Wayland game driver = Auto (8xx owners: try the alternatives one by one),
   FEXCore = an installed version. DXVK / VKD3D / components / audio as on X11.
4. Optional experiments via Env Vars: BANNER_WAYLAND_ZERO_COPY=1 (fullscreen games only),
   BANNER_WAYLAND_UBWC=0 (if the picture is scrambled), TU_DEBUG=sysmem (Vauzi's tip for 710/720/722).

Verified on this build (AYANEO Pocket FIT, Adreno 750)
-----------------------------------------------------
  All eight game drivers load their own manifest and render the AIO Graphics Test on Wayland.
  Switch sweep Vulkan -> D3D12 -> D3D9 alive in one launch.
  Half-Life 2: desktop survives the launch; touch mouse-look under a pointer lock; 144 fps.
  Zero-copy on: 1239 of 1240 presented frames without a copy, picture correct, same 144 fps cap.
  First launch with a stale prefix: 1280x720, no resize.
  Notepad: paste from Android, type, copy back; soft keyboard auto-opens.

Known gaps
----------
- Frame rate is still not ahead of X11 on synthetic tests (the cap hides the zero-copy gain here).
- Real Adreno 710/720/722 and 830/840 hardware untested: please report which a8xx build works best.
- Frame generation, drag-and-drop, image clipboard, window decorations: not on Wayland yet.

Logs: Download/Wayland-logs/wayland-*.log (compositor) and Download/bannerlator/<game>/wine_debug.log.
