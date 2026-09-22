package com.winlator.star.linux;

import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The performance switches a Linux session runs with, and the file that overrides them.
 *
 * <p>The client's interface is not short of CPU or GPU: measured on an Adreno 840, menus ran at
 * ~14 fps with the core override confirmed applied while a game on the same device ran at 89. The
 * cost is the chain that draws the menu — Chromium to ANGLE to Zink to Turnip — so these are the
 * switches that make that chain cheaper, not ones that ask for more hardware.
 *
 * <p>Each is written into {@code Download/Bannerlator-LinuxSteam/linux-tuning.conf} the first time
 * a session starts, commented with what it does, and read back on every launch. That is what makes
 * a comparison possible without a build: change one line, start a session, read the fps. The file
 * wins over the defaults here, {@code KEY=} with nothing after it removes a variable entirely, and
 * every value that ends up in effect is written to the session log so a result can be matched to
 * the settings that produced it.
 */
public final class LinuxTuning {
    private static final String TAG = "LinuxTuning";
    private static final String FILE = "linux-tuning.conf";

    /** Marks the client's own command line rather than the session environment. */
    public static final String STEAMDECK = "BL_STEAMDECK";

    private LinuxTuning() {}

    /**
     * What a session runs with when the file says nothing.
     *
     * <p>None of these is device-proven yet; they are the ranked candidates, on by default so the
     * comparison starts from them and a tester who changes nothing is still testing something.
     */
    private static Map<String, String> defaults() {
        Map<String, String> env = new LinkedHashMap<>();
        // Zink's GL front end marshals on the calling thread. glthread moves that to a second one,
        // which is the shape of this bottleneck: CPU-bound, single-threaded, enormous call counts.
        env.put("mesa_glthread", "true");
        // The cheaper descriptor path. Zink's default re-validates far more than this UI needs.
        env.put("ZINK_DESCRIPTORS", "lazy");
        // Skips GL error bookkeeping in the hot path. Small, and free.
        env.put("MESA_NO_ERROR", "1");
        // Steam's own Deck mode: SteamOS runs the client as `-steamdeck -steamos3`, which is the
        // configuration Valve tunes Big Picture for. Off by default because it also makes the
        // client expect Deck hardware — battery sysfs, TDP controls, a Deck input device — none of
        // which exists here, and the effect of that is untested.
        env.put(STEAMDECK, "0");
        return env;
    }

    private static File file() {
        return new File(LinuxRuntime.debugLogDir(), FILE);
    }

    /** The values in effect: the defaults, with the file's lines applied over them. */
    public static Map<String, String> effective() {
        Map<String, String> env = defaults();
        File f = file();
        if (!f.isFile()) {
            writeTemplate(f, env);
            return env;
        }
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String line;
            while ((line = r.readLine()) != null) {
                String s = line.trim();
                if (s.isEmpty() || s.startsWith("#")) continue;
                int eq = s.indexOf('=');
                if (eq <= 0) continue;
                String key = s.substring(0, eq).trim();
                String value = s.substring(eq + 1).trim();
                if (key.isEmpty()) continue;
                // A key with nothing after the = takes the variable out of the session, which is
                // the other half of the comparison: a default has to be removable, not only changed.
                if (value.isEmpty()) env.remove(key);
                else env.put(key, value);
            }
        } catch (Exception e) {
            Log.w(TAG, "could not read " + f + "; using the defaults", e);
            return defaults();
        }
        return env;
    }

    /**
     * Adds the environment half to a session's guest environment.
     *
     * <p>{@link #STEAMDECK} is not an environment variable the guest reads for its own sake — the
     * session script turns it into the client's command line — but it travels the same way.
     */
    public static void apply(List<String> guest) {
        for (Map.Entry<String, String> e : effective().entrySet()) {
            guest.add(e.getKey() + "=" + e.getValue());
        }
    }

    /** One line per value, for the session log, so a measurement names what produced it. */
    public static String report() {
        StringBuilder b = new StringBuilder();
        for (Map.Entry<String, String> e : effective().entrySet()) {
            b.append(String.format("%-24s", e.getKey())).append(e.getValue()).append('\n');
        }
        return b.toString();
    }

    private static void writeTemplate(File f, Map<String, String> env) {
        try {
            //noinspection ResultOfMethodCallIgnored
            f.getParentFile().mkdirs();
            try (FileWriter w = new FileWriter(f)) {
                w.write("# Bannerlator — Linux session performance switches.\n");
                w.write("#\n");
                w.write("# Change a line, start a session, read the fps on the HUD. Change one at a\n");
                w.write("# time: two at once and neither result means anything.\n");
                w.write("#\n");
                w.write("# KEY=value   sets it       KEY=   (nothing after the =) removes it\n");
                w.write("# A line starting with # is ignored. Every value in effect is written to\n");
                w.write("# the session log, so a result can be matched to what produced it.\n");
                w.write("#\n");
                w.write("# mesa_glthread    true  — GL call marshalling on its own thread. The menu\n");
                w.write("#                          is CPU-bound in exactly this, so it is the most\n");
                w.write("#                          likely of these to move the number.\n");
                w.write("# ZINK_DESCRIPTORS lazy  — the cheaper descriptor path.\n");
                w.write("# MESA_NO_ERROR    1     — skip GL error bookkeeping.\n");
                w.write("# BL_STEAMDECK     0/1   — run the client the way SteamOS does\n");
                w.write("#                          (-steamdeck -steamos3). Off: it makes the client\n");
                w.write("#                          expect Deck hardware that is not here, and what\n");
                w.write("#                          that costs is untested.\n");
                w.write("\n");
                for (Map.Entry<String, String> e : env.entrySet()) {
                    w.write(e.getKey() + "=" + e.getValue() + "\n");
                }
            }
            Log.i(TAG, "wrote the tuning template to " + f);
        } catch (Exception e) {
            Log.w(TAG, "could not write " + f, e);
        }
    }
}
