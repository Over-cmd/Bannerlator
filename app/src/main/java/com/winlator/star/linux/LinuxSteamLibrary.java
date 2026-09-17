package com.winlator.star.linux;

import android.util.Log;

import com.winlator.star.container.Container;
import com.winlator.star.core.FileUtils;

import java.io.File;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Presents the games this app already downloaded to the native Steam client, so the same install
 * serves both launchers and nothing is downloaded twice.
 *
 * <p>The app writes a real Steam layout into the container's prefix —
 * {@code drive_c/Program Files (x86)/Steam/steamapps} with {@code appmanifest_<appid>.acf} beside
 * {@code common/<InstallDir>} — so the client only needs that directory registered as a library
 * folder. The manifests are copied into the rootfs (they are tiny), and every game directory is
 * <em>bind-mounted</em> rather than copied: one copy on disk, visible to both.
 *
 * <p>Ported from WinNative's gamescope runtime (GPL-3.0).
 */
public final class LinuxSteamLibrary {
    private static final String TAG = "LinuxSteamLibrary";

    /** Where the library appears inside the runtime. */
    public static final String GUEST_ROOT = "/mnt/bannerlator";

    private static final Pattern MANIFEST = Pattern.compile("appmanifest_(\\d+)\\.acf");
    /** {@code "installdir"  "Half-Life 2"} — the folder the game lives in under common/. */
    private static final Pattern INSTALLDIR =
            Pattern.compile("\"installdir\"\\s*\"([^\"]+)\"", Pattern.CASE_INSENSITIVE);

    private LinuxSteamLibrary() {}

    /** The container prefix directory the app installs Steam games into. */
    public static File prefixSteamApps(Container container) {
        return new File(container.getRootDir(),
                ".wine/drive_c/Program Files (x86)/Steam/steamapps");
    }

    /**
     * Builds the library inside {@code rootfs} from {@code container}'s installed games and returns
     * the proot bind specs ({@code host:guest}) that put each game in place. Worker thread: this
     * reads and copies files.
     */
    public static List<String> prepare(Container container, File rootfs) {
        List<String> binds = new ArrayList<>();
        if (container == null) return binds;

        File source = prefixSteamApps(container);
        File[] manifests = source.listFiles((dir, name) -> MANIFEST.matcher(name).matches());
        if (manifests == null || manifests.length == 0) return binds;

        File steamapps = new File(rootfs, GUEST_ROOT.substring(1) + "/steamapps");
        File common = new File(steamapps, "common");
        if (!common.isDirectory() && !common.mkdirs()) {
            Log.w(TAG, "cannot create " + common);
            return binds;
        }

        Set<String> kept = new HashSet<>();
        for (File manifest : manifests) {
            String text = FileUtils.readString(manifest);
            if (text == null) continue;
            Matcher m = INSTALLDIR.matcher(text);
            if (!m.find()) continue;
            String installDir = m.group(1);
            File gameDir = new File(source, "common/" + installDir);
            // A manifest whose game folder is gone would show in Steam as installed and fail to
            // launch, which is worse than not showing it at all.
            if (!gameDir.isDirectory()) continue;

            if (!FileUtils.copy(manifest, new File(steamapps, manifest.getName()))) continue;
            kept.add(manifest.getName());
            new File(common, installDir).mkdirs();
            binds.add(gameDir.getPath() + ":" + GUEST_ROOT + "/steamapps/common/" + installDir);
        }

        // Drop manifests for games that have since been uninstalled or moved.
        File[] stale = steamapps.listFiles((dir, name) -> MANIFEST.matcher(name).matches());
        if (stale != null) {
            for (File file : stale) {
                if (!kept.contains(file.getName())) file.delete();
            }
        }
        Log.i(TAG, "exposed " + binds.size() + " installed game(s) to the Steam client");
        return binds;
    }
}
