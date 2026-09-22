package com.winlator.star.linux;

import com.winlator.star.container.Shortcut;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The performance switches a Linux session runs with, as set per entry in the Steam (Linux)
 * settings.
 *
 * <p>The client's interface is not short of CPU or GPU: measured on an Adreno 840, its menus ran at
 * ~14 fps with the core override confirmed applied while a game on the same device ran at 89. The
 * cost is the chain that draws the menu — Chromium to ANGLE to Zink to Turnip — so these make that
 * chain cheaper rather than asking for more hardware.
 *
 * <p>Each switch is one extra on the entry, {@code "1"} or {@code "0"}, empty meaning the default
 * below. Three set an environment variable for the session; {@link #EXTRA_STEAMDECK} is turned by
 * the session script into the client's own command line instead. Whatever ends up in effect is
 * written into the session's {@code device.txt}, so a measurement names what produced it.
 *
 * <p>None of the defaults is device-proven — they are the ranked candidates. Arbitrary variables
 * beyond these belong in the entry's own "Env vars" field, which is applied after these.
 */
public final class LinuxTuning {
    /** Zink's GL front end marshals on the calling thread; this moves that to a second one. */
    public static final String EXTRA_GLTHREAD = "linuxGlThread";
    /** The cheaper Zink descriptor path. */
    public static final String EXTRA_LAZY_DESCRIPTORS = "linuxLazyDescriptors";
    /** Skips GL error bookkeeping in the hot path. */
    public static final String EXTRA_NO_GL_ERROR = "linuxNoGlError";
    /** Runs the client the way SteamOS runs its own session. */
    public static final String EXTRA_STEAMDECK = "linuxSteamDeckMode";

    private LinuxTuning() {}

    /**
     * Whether a switch is on when the entry has never been edited.
     *
     * <p>The three environment ones are on so a tester who changes nothing is still testing them.
     * Deck mode is off: it makes the client expect Deck hardware — battery sysfs, TDP controls, a
     * Deck input device — none of which is here, and what that costs has not been measured.
     */
    public static boolean defaultOn(String extra) {
        return !EXTRA_STEAMDECK.equals(extra);
    }

    /** A switch's state for this entry: its own value, or the default when it has none. */
    public static boolean isOn(Shortcut shortcut, String extra) {
        String v = shortcut != null ? shortcut.getExtra(extra, "") : "";
        if (v == null || v.isEmpty()) return defaultOn(extra);
        return "1".equals(v);
    }

    /** What the switches come to: the environment half, in the order they are shown. */
    public static Map<String, String> environment(Shortcut shortcut) {
        Map<String, String> env = new LinkedHashMap<>();
        if (isOn(shortcut, EXTRA_GLTHREAD)) env.put("mesa_glthread", "true");
        if (isOn(shortcut, EXTRA_LAZY_DESCRIPTORS)) env.put("ZINK_DESCRIPTORS", "lazy");
        if (isOn(shortcut, EXTRA_NO_GL_ERROR)) env.put("MESA_NO_ERROR", "1");
        return env;
    }

    /**
     * Adds the switches to a session's guest environment.
     *
     * <p>Deck mode travels as {@code BL_STEAMDECK} because the session script, not the guest,
     * is what turns it into {@code -steamdeck -steamos3} on the client's command line.
     */
    public static void apply(List<String> guest, Shortcut shortcut) {
        for (Map.Entry<String, String> e : environment(shortcut).entrySet()) {
            guest.add(e.getKey() + "=" + e.getValue());
        }
        guest.add("BL_STEAMDECK=" + (isOn(shortcut, EXTRA_STEAMDECK) ? "1" : "0"));
    }

    /** One line per switch for the session's device report, so a number names its settings. */
    public static String report(Shortcut shortcut) {
        String[][] rows = {
                {"Threaded GL", EXTRA_GLTHREAD},
                {"Lazy descriptors", EXTRA_LAZY_DESCRIPTORS},
                {"Skip GL error checks", EXTRA_NO_GL_ERROR},
                {"Steam Deck mode", EXTRA_STEAMDECK},
        };
        StringBuilder b = new StringBuilder();
        for (String[] row : rows) {
            boolean on = isOn(shortcut, row[1]);
            String raw = shortcut != null ? shortcut.getExtra(row[1], "") : "";
            boolean set = raw != null && !raw.isEmpty();
            b.append(String.format("%-24s", row[0])).append(on ? "on" : "off")
             .append(set ? "" : " (default)").append('\n');
        }
        return b.toString();
    }
}
