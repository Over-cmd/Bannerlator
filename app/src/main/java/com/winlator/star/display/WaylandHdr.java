package com.winlator.star.display;

import android.content.Context;
import android.hardware.display.DisplayManager;
import android.view.Display;

import com.winlator.star.container.Container;
import com.winlator.star.container.Shortcut;
import com.winlator.star.contents.ContentProfile;
import com.winlator.star.contents.ContentsManager;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * The "HDR output" setting of the Wayland backend — one place for everything the container editor,
 * the game-shortcut editor, XMB game settings and the launch path must agree on.
 *
 * <p><b>What it does.</b> On a screen that reports HDR10, a game that supports HDR gets it: the Wayland
 * compositor offers games HDR10 (wp_color_manager_v1 + 10-bit buffers) and puts their frames on their
 * own display layer tagged BT.2020 PQ (see {@code waylandcomp/src/banner_color.h}), and the session
 * exports {@code DXVK_HDR=1} so DXVK tells the game the display is HDR. The game still needs HDR
 * switched on in its own settings. Not the drawer's "HDR" screen effect (an SDR bloom filter).
 *
 * <p><b>Where it is stored.</b> {@link #EXTRA} in the container's extraData ("1" = on, absent = off) and
 * in a game shortcut's extras ("1" on, "0" off, absent / "" = the container's). The shortcut wins, the
 * same resolve the launch path uses ({@link #effective}). {@code BANNER_WAYLAND_HDR} in the container's
 * or shortcut's environment variables overrides both (1 / 0 / force) — kept for testing.
 *
 * <p><b>When it can be on.</b> Only where the screen the game runs on lists HDR10
 * ({@link #unavailableReason}); the editors grey the row out with the reason everywhere else, and the
 * launch path turns nothing on there even if the stored value says on.
 *
 * <p>Every method is safe to call from any thread and never throws; {@link #layerVersionCode} scans the
 * installed contents on its first call per layer, so editors should call it off the main thread.
 */
public final class WaylandHdr {
    private WaylandHdr() {}

    /** The extra's key on the container and on a shortcut. */
    public static final String EXTRA = "waylandHdr";

    /** First versionCode of the Wayland layer whose winevulkan exposes VK_EXT_swapchain_colorspace, which
     *  DXVK 2.x needs before it lists any HDR colour space (DXVK 3.x does not ask for it). */
    public static final int LAYER_VERSION_CODE_DXVK2_HDR = 10;

    public static final String TITLE = "HDR output (HDR10)";

    /** The setting's help text (every editor shows the same words). */
    public static final String HELP_TEXT =
            "Games that support HDR show it on this screen: their frames go to the display in 10-bit with " +
            "the HDR10 (BT.2020 PQ) tag, on their own display layer. Switch HDR on in the game's own " +
            "settings too. While a game shows HDR, screen effects and frame generation are paused for it. " +
            "Windows is told this screen's peak brightness as Android reports it; Android gives no live " +
            "brightness in nits, so the session log records the live HDR/SDR headroom instead. " +
            "Not the same as the \"HDR\" screen effect.";

    /** The screen a game started from an editor will run on: the device's built-in display. */
    public static Display targetDisplay(Context context) {
        try {
            DisplayManager dm = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
            return dm != null ? dm.getDisplay(Display.DEFAULT_DISPLAY) : null;
        } catch (Throwable t) {
            return null;
        }
    }

    /** {@code null} when {@code display} can show HDR10, else the reason it cannot (for a greyed row). */
    public static String unavailableReason(Display display) {
        DisplayHdrInfo d = DisplayHdrInfo.read(display);
        if (d.supportsHdr10) return null;
        if ("unknown".equals(d.formats)) return "This screen does not report its HDR capability, so HDR output stays off.";
        if ("none".equals(d.formats)) return "This screen does not support HDR (it reports no HDR types), so HDR output stays off.";
        return "This screen does not support HDR10 (it reports " + d.formats + "), so HDR output stays off.";
    }

    /** {@link #unavailableReason(Display)} for the device's built-in screen. */
    public static String unavailableReason(Context context) {
        return unavailableReason(targetDisplay(context));
    }

    /** The shortcut's own choice: "1" on, "0" off, "" = use the container's. */
    public static String shortcutChoice(Shortcut shortcut) {
        if (shortcut == null) return "";
        String v = shortcut.getExtra(EXTRA, "");
        return "1".equals(v) || "0".equals(v) ? v : "";
    }

    /** The stored setting for a game: the shortcut's own choice, else the container's. */
    public static boolean effective(Shortcut shortcut, Container container) {
        String s = shortcutChoice(shortcut);
        if (!s.isEmpty()) return s.equals("1");
        return container != null && container.isWaylandHdr();
    }

    /** Major version of the DXVK a dxwrapperConfig names ("version=2.4.1-1-gplasync-…" -> 2,
     *  "version=v3.1-gplasync-0" -> 3); -1 when it names none or cannot be read. */
    public static int dxvkMajor(String dxwrapperConfig) {
        if (dxwrapperConfig == null) return -1;
        String version = null;
        for (String part : dxwrapperConfig.split(",")) {
            int eq = part.indexOf('=');
            if (eq > 0 && part.substring(0, eq).trim().equals("version")) { version = part.substring(eq + 1).trim(); break; }
        }
        if (version == null) return -1;
        int i = 0;
        while (i < version.length() && !Character.isDigit(version.charAt(i))) i++;
        int j = i;
        while (j < version.length() && Character.isDigit(version.charAt(j))) j++;
        if (j == i) return -1;
        try { return Integer.parseInt(version.substring(i, j)); } catch (NumberFormatException e) { return -1; }
    }

    private static final Map<String, Integer> layerCodes = new HashMap<>();

    /** The installed layer's versionCode as its profile declares it (profile.json), -1 when unknown.
     *  Cached per identifier; the first call per layer scans the installed contents. */
    public static int layerVersionCode(Context context, String wineVersion) {
        if (context == null || wineVersion == null || wineVersion.isEmpty()) return -1;
        synchronized (layerCodes) {
            Integer cached = layerCodes.get(wineVersion);
            if (cached != null) return cached;
        }
        int code = -1;
        try {
            ContentsManager cm = new ContentsManager(context);
            cm.syncContents();
            ContentProfile p = cm.getProfileByEntryName(wineVersion);
            if (p != null) code = p.verCode;
        } catch (Throwable ignored) {}
        synchronized (layerCodes) { layerCodes.put(wineVersion, code); }
        return code;
    }

    /**
     * The DXVK 2.x warning, or {@code null} when none applies: DXVK before 3.0 lists HDR colour spaces
     * only when the instance exposes VK_EXT_swapchain_colorspace, which the Wayland layer's winevulkan
     * does from versionCode {@link #LAYER_VERSION_CODE_DXVK2_HDR}. Decided by the layer's versionCode,
     * never its name; an unknown versionCode or DXVK version gives no warning.
     */
    public static String dxvkWarning(Context context, String wineVersion, String dxwrapper, String dxwrapperConfig) {
        if (dxwrapper != null && !dxwrapper.isEmpty() && !dxwrapper.toLowerCase(Locale.ROOT).contains("dxvk")) return null;
        int dxvk = dxvkMajor(dxwrapperConfig);
        if (dxvk < 0 || dxvk >= 3) return null;
        int layer = layerVersionCode(context, wineVersion);
        if (layer < 0 || layer >= LAYER_VERSION_CODE_DXVK2_HDR) return null;
        return "DXVK " + dxvk + ".x cannot offer games HDR on this layer (versionCode " + layer + "): use DXVK v3.1, "
                + "or a Wayland layer of versionCode " + LAYER_VERSION_CODE_DXVK2_HDR + " or newer.";
    }

    /** Decimal nits for an env var: "1351", "0.05", "0" — no exponent, no trailing zeros; null for < 0. */
    public static String nits(float v) {
        if (v < 0f || Float.isNaN(v) || Float.isInfinite(v)) return null;
        String s = String.format(Locale.US, "%.4f", v);
        if (s.indexOf('.') >= 0) {
            s = s.replaceAll("0+$", "");
            if (s.endsWith(".")) s = s.substring(0, s.length() - 1);
        }
        return s;
    }
}
