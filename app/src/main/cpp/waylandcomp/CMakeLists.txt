# ── Embedded Wayland compositor (experimental parallel runtime) ──────────────
# Runs a libwayland-server compositor in-process so games launched via Wine's
# winewayland.drv can present into it. Protocol glue is pre-generated in
# generated/ (no host wayland-scanner needed at app build time). Built by the app
# (add_subdirectory from ../CMakeLists.txt) and on its own by standalone/ for
# build-waylandcomp.yml.
add_library(bannerwayland SHARED
        src/compositor.c
        src/vk_present.c
        src/vk_loader.c
        src/waylandcomp_jni.c
        src/banner_ext.c
        src/wl_clipboard.c
        src/wl_text_input.c
        src/wl_toplevel_icon.c
        generated/xdg-shell-protocol.c
        generated/linux-dmabuf-v1-protocol.c
        generated/viewporter-protocol.c
        generated/banner-desktop-v1-protocol.c
        generated/presentation-time-protocol.c
        generated/pointer-constraints-unstable-v1-protocol.c
        generated/relative-pointer-unstable-v1-protocol.c
        generated/wlr-data-control-unstable-v1-protocol.c
        generated/text-input-unstable-v3-protocol.c
        generated/xdg-toplevel-icon-v1-protocol.c
)
target_include_directories(bannerwayland PRIVATE
        generated
        prebuilt/include
        src
)
add_library(wayland-server SHARED IMPORTED)
set_target_properties(wayland-server PROPERTIES IMPORTED_LOCATION
        ${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/lib/libwayland-server.so)
# Vulkan is loaded at runtime via adrenotools (Turnip), not linked directly — so the
# compositor uses Turnip's driver, which supports dmabuf import (drm_format_modifier).
target_link_libraries(bannerwayland
        wayland-server
        adrenotools
        dl
        log
        android
)
