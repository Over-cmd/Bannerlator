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
    /**
     * The app's installed Steam games, presented to the Linux client as one library folder.
     *
     * The store's database is the source of truth: one row per game with the folder it was
     * actually installed to, kept current when a game is moved between the app's storage and a
     * card. The links the store leaves under a container's prefix are copies of that and go stale
     * on a move, which is why they are not followed here. A folder the database does not know is
     * still taken if it identifies itself: the Steam API layer leaves steam_appid.txt in a folder
     * that has been launched, and the downloader's journal names the depots it installed, which the
     * database maps back to an app.
     *
     * A container's prefix manifest is still preferred as the manifest handed to the client when
     * one exists, because it carries the build the app installed; otherwise one is written from
     * the row. Builds are reconciled both ways with the prefix manifest that owns the title.
     */
    public static List<String> prepare(android.content.Context context, List<Container> containers, File rootfs) {
        List<String> binds = new ArrayList<>();
        File steamapps = new File(rootfs, GUEST_ROOT.substring(1) + "/steamapps");
        File common = new File(steamapps, "common");
        if (!common.isDirectory() && !common.mkdirs()) {
            Log.w(TAG, "cannot create " + common);
            return binds;
        }
        Set<String> kept = new HashSet<>();
        java.util.Map<String, File> prefixManifests = prefixManifests(containers);

        // 1. What the database says is installed.
        java.util.Map<Integer, String> depotToApp = new java.util.HashMap<>();
        Set<Integer> known = new HashSet<>();
        try {
            for (com.winlator.star.store.SteamDatabase.GameRow row
                    : com.winlator.star.store.SteamDatabase.getInstance(context).getInstalledGames()) {
                if (row.depotIds != null) {
                    for (String d : row.depotIds.split(",")) {
                        try { depotToApp.put(Integer.parseInt(d.trim()), String.valueOf(row.appId)); } catch (NumberFormatException ignored) {}
                    }
                }
                File dir = installDirOf(context, row);
                if (!dir.isDirectory()) {
                    Log.w(TAG, row.name + " (" + row.appId + ") is recorded at " + dir + " but the folder is missing");
                    continue;
                }
                known.add(row.appId);
                expose(String.valueOf(row.appId), row.name, dir, steamapps, common, kept, binds, prefixManifests);
            }
        } catch (Throwable t) {
            Log.w(TAG, "installed games unavailable from the database", t);
        }

        // 2. Folders that identify themselves and that the database missed.
        for (File root : storeRoots(context)) {
            File[] dirs = root.listFiles(File::isDirectory);
            if (dirs == null) continue;
            for (File dir : dirs) {
                String appId = appIdOfFolder(dir, depotToApp);
                if (appId == null || known.contains(Integer.parseInt(appId))) continue;
                Log.i(TAG, dir.getName() + " identifies itself as " + appId + " without a database row");
                expose(appId, dir.getName(), dir, steamapps, common, kept, binds, prefixManifests);
            }
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

    /** The launching container alone; kept for callers that have no context. */
    public static List<String> prepare(Container container, File rootfs) {
        return new ArrayList<>();
    }

    /** Same rule the updater uses: the recorded folder, else the app's own storage by name. */
    private static File installDirOf(android.content.Context context,
                                     com.winlator.star.store.SteamDatabase.GameRow row) {
        if (row.installDir != null && !row.installDir.isEmpty()) return new File(row.installDir);
        return new File(new File(context.getFilesDir(), "imagefs/steam_games"), safeName(row.name));
    }

    private static String safeName(String name) {
        String safe = name == null ? "" : name.replaceAll("[/\\\\:*?\"<>|]", "_").trim();
        return safe.isEmpty() ? "game" : safe;
    }

    /** The app's own steam_games and bannerlator/steam_games on every mounted volume. */
    private static List<File> storeRoots(android.content.Context context) {
        List<File> roots = new ArrayList<>();
        roots.add(new File(context.getFilesDir(), "imagefs/steam_games"));
        File[] volumes = new File("/storage").listFiles();
        if (volumes != null) {
            for (File volume : volumes) roots.add(new File(volume, "bannerlator/steam_games"));
        }
        return roots;
    }

    /** steam_appid.txt from a launch, else the downloader's journal mapped through the database. */
    private static String appIdOfFolder(File dir, java.util.Map<Integer, String> depotToApp) {
        String text = FileUtils.readString(new File(dir, "steam_appid.txt"));
        if (text != null && text.trim().matches("\\d+")) return text.trim();
        String journal = FileUtils.readString(new File(dir, ".bl_depot/depot.config"));
        if (journal == null) return null;
        Matcher m = Pattern.compile("\"(\\d+)\"\\s*:").matcher(journal);
        while (m.find()) {
            String app = depotToApp.get(Integer.parseInt(m.group(1)));
            if (app != null) return app;
        }
        return null;
    }

    /** Every container's prefix manifests by file name; the first container that has one wins. */
    private static java.util.Map<String, File> prefixManifests(List<Container> containers) {
        java.util.Map<String, File> found = new java.util.HashMap<>();
        if (containers == null) return found;
        for (Container container : containers) {
            File[] manifests = prefixSteamApps(container).listFiles((dir, name) -> MANIFEST.matcher(name).matches());
            if (manifests == null) continue;
            for (File m : manifests) found.putIfAbsent(m.getName(), m);
        }
        return found;
    }

    private static void expose(String appId, String name, File dir, File steamapps, File common,
                               Set<String> kept, List<String> binds, java.util.Map<String, File> prefixManifests) {
        String manifestName = "appmanifest_" + appId + ".acf";
        if (kept.contains(manifestName)) return;
        String realDir;
        try {
            realDir = dir.getCanonicalPath();
        } catch (java.io.IOException e) {
            Log.w(TAG, "cannot resolve " + dir, e);
            return;
        }
        String installDir = dir.getName();
        File runtimeManifest = new File(steamapps, manifestName);
        File prefixManifest = prefixManifests.get(manifestName);
        if (prefixManifest != null) {
            // Both sides update titles: adopt the client's build into the app's copy when it is
            // newer, and replace the client's only when the app holds the newer build.
            long runtimeBuild = buildId(runtimeManifest), appBuild = buildId(prefixManifest);
            if (runtimeBuild > appBuild) {
                if (!FileUtils.copy(runtimeManifest, prefixManifest)) Log.w(TAG, "could not adopt " + runtimeManifest);
            } else if (runtimeBuild == 0L || appBuild > runtimeBuild) {
                if (!FileUtils.copy(prefixManifest, runtimeManifest)) return;
            }
        } else if (!runtimeManifest.isFile()) {
            com.winlator.star.store.RealSteamLauncher.writeAppManifest(
                    runtimeManifest, Integer.parseInt(appId), name, installDir, 0L);
        }
        kept.add(manifestName);
        new File(common, installDir).mkdirs();
        binds.add(realDir + ":" + GUEST_ROOT + "/steamapps/common/" + installDir);
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
