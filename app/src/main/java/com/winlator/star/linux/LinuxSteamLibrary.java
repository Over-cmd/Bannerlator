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
    /** The launching container alone; kept for callers that have no manager. */
    public static List<String> prepare(Container container, File rootfs) {
        List<Container> one = new ArrayList<>();
        if (container != null) one.add(container);
        return prepare(one, rootfs);
    }

    /**
     * Every container's installs, not just the launching one's. The app's store installs into
     * whichever container the user chose for a title, and the Linux client's own entry lives in a
     * container of its own with no store installs at all - so reading only that one presented the
     * client with an empty library on a device holding twenty installed games across three others.
     * A title installed in more than one container is taken from the first that has it complete.
     */
    public static List<String> prepare(List<Container> containers, File rootfs) {
        List<String> binds = new ArrayList<>();
        if (containers == null || containers.isEmpty()) return binds;
        File steamapps = new File(rootfs, GUEST_ROOT.substring(1) + "/steamapps");
        File common = new File(steamapps, "common");
        if (!common.isDirectory() && !common.mkdirs()) {
            Log.w(TAG, "cannot create " + common);
            return binds;
        }
        Set<String> kept = new HashSet<>();
        for (Container container : containers) {
            prepareContainer(container, steamapps, common, kept, binds);
        }
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
     * The store's install roots: the app's own, and a bannerlator/steam_games folder on every
     * mounted volume, which is where the storage picker puts a card.
     */
    private static File findMovedGame(File source, String installDir) {
        List<File> roots = new ArrayList<>();
        // <imagefs>/steam_games sits four levels above <prefix>/steamapps: .wine/drive_c/Program Files (x86)/Steam/steamapps
        File prefixRoot = source;
        for (int i = 0; i < 5 && prefixRoot != null; i++) prefixRoot = prefixRoot.getParentFile();
        File home = prefixRoot != null ? prefixRoot.getParentFile() : null;       // imagefs/home/xuser-N -> imagefs/home
        File imagefs = home != null ? home.getParentFile() : null;                // imagefs
        if (imagefs != null) roots.add(new File(imagefs, "steam_games"));
        File storage = new File("/storage");
        File[] volumes = storage.listFiles();
        if (volumes != null) {
            for (File volume : volumes) {
                roots.add(new File(volume, "bannerlator/steam_games"));
            }
        }
        for (File root : roots) {
            File candidate = new File(root, installDir);
            if (candidate.isDirectory()) return candidate;
        }
        return null;
    }

    private static final Pattern STATE =
            Pattern.compile("^\\s*\"StateFlags\"\\s*\"(\\d+)\"", Pattern.MULTILINE);

    private static void prepareContainer(Container container, File steamapps, File common,
                                         Set<String> kept, List<String> binds) {
        if (container == null) return;
        File source = prefixSteamApps(container);
        File[] manifests = source.listFiles((dir, name) -> MANIFEST.matcher(name).matches());
        if (manifests == null) return;
        for (File manifest : manifests) {
            if (kept.contains(manifest.getName())) continue;
            String text = FileUtils.readString(manifest);
            if (text == null) continue;
            // Only a complete install: the store writes StateFlags 4 for one; 6 and 1026 are
            // still downloading or waiting on an update, and a bind of those would offer the
            // client half a game.
            Matcher st = STATE.matcher(text);
            if (st.find() && !"4".equals(st.group(1))) continue;
            Matcher m = INSTALLDIR.matcher(text);
            if (!m.find()) continue;
            String installDir = m.group(1);
            File gameDir = new File(source, "common/" + installDir);
            if (!gameDir.isDirectory()) {
                // The prefix link is stale: the store moved the game between storages and did not
                // repoint it. The folder keeps its name wherever it went, so look for it under the
                // store's roots before giving up. Only a dangling link is recovered this way; a
                // link that resolves is trusted as it stands.
                gameDir = findMovedGame(source, installDir);
                if (gameDir == null) continue;
                Log.i(TAG, installDir + ": prefix link is stale, found at " + gameDir);
            }
            File runtimeManifest = new File(steamapps, manifest.getName());
            long runtimeBuild = buildId(runtimeManifest), appBuild = buildId(manifest);
            if (runtimeBuild > appBuild) {
                if (!FileUtils.copy(runtimeManifest, manifest)) Log.w(TAG, "could not adopt " + runtimeManifest);
            } else if (runtimeBuild == 0L || appBuild > runtimeBuild) {
                if (!FileUtils.copy(manifest, runtimeManifest)) continue;
            }
            // The prefix entry is a link to wherever the store put the files - the app's own
            // storage or a card the user chose. A card is not one of the trees bound into the
            // runtime, so binding the link itself hands the client a folder that resolves to
            // nothing inside. The real folder is bound instead, wherever it is.
            String realDir;
            try {
                realDir = gameDir.getCanonicalPath();
            } catch (java.io.IOException e) {
                Log.w(TAG, "cannot resolve " + gameDir, e);
                continue;
            }
            kept.add(manifest.getName());
            new File(common, installDir).mkdirs();
            binds.add(realDir + ":" + GUEST_ROOT + "/steamapps/common/" + installDir);
        }
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
