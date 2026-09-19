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

    private static final Pattern BUILD_ID =
            Pattern.compile("^\\s*\"buildid\"\\s*\"(\\d+)\"", Pattern.MULTILINE);
    private static final Pattern NAME =
            Pattern.compile("^\\s*\"name\"\\s*\"([^\"]+)\"", Pattern.MULTILINE);

    private LinuxSteamLibrary() {}

    /** The build a manifest records, or 0 when there is no readable manifest. */
    private static long buildId(File manifest) {
        String text = manifest.isFile() ? FileUtils.readString(manifest) : null;
        if (text == null) return 0L;
        Matcher m = BUILD_ID.matcher(text);
        return m.find() ? Long.parseLong(m.group(1)) : 0L;
    }

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

            // Both sides update titles, so the manifest is reconciled rather than rewritten.
            // A build the client installed is adopted into the app's copy, and the app's copy only
            // replaces the client's when the app holds the newer build. (WinNative ffd622df.)
            File runtimeManifest = new File(steamapps, manifest.getName());
            long runtimeBuild = buildId(runtimeManifest), appBuild = buildId(manifest);
            if (runtimeBuild > appBuild) {
                if (!FileUtils.copy(runtimeManifest, manifest)) Log.w(TAG, "could not adopt " + runtimeManifest);
            } else if (runtimeBuild == 0L || appBuild > runtimeBuild) {
                if (!FileUtils.copy(manifest, runtimeManifest)) continue;
            }
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

    /**
     * The other direction: a game the Linux client installed for itself gets an entry in the Games
     * tab that launches it through the client. Nothing is copied into the Wine prefix, because the
     * game lives in the runtime and runs on ARM64 Proton there, not on Wine; the entry carries the
     * app id and the client's own title. Entries are only ever added, never removed here: a game
     * the client has since uninstalled keeps its card until the user removes it, which is how the
     * app's own store behaves too.
     */
    public static int syncClientGames(Container container, File rootfs) {
        if (container == null) return 0;
        File steamapps = new File(rootfs, "root/.local/share/Steam/steamapps");
        File[] manifests = steamapps.listFiles((dir, name) -> MANIFEST.matcher(name).matches());
        if (manifests == null) return 0;
        int added = 0;
        for (File manifest : manifests) {
            Matcher id = MANIFEST.matcher(manifest.getName());
            if (!id.matches()) continue;
            String appId = id.group(1);
            String text = FileUtils.readString(manifest);
            if (text == null) continue;
            Matcher name = NAME.matcher(text);
            Matcher dir = INSTALLDIR.matcher(text);
            if (!name.find() || !dir.find()) continue;
            // The client's own tools are manifests too; only a title with an installed folder that
            // is not one of them is a game.
            if (!new File(steamapps, "common/" + dir.group(1)).isDirectory()) continue;
            String title = name.group(1);
            if (title.startsWith("Proton") || title.startsWith("Steam Linux Runtime")
                    || title.startsWith("Steamworks") || title.equals("FEX")) continue;
            if (LinuxShortcuts.createClientGameShortcut(container, appId, title)) added++;
        }
        if (added > 0) Log.i(TAG, "added " + added + " game(s) the Linux client installed");
        return added;
    }
}
