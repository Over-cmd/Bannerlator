package com.winlator.star.linux;

import android.content.Context;
import android.graphics.BitmapFactory;
import android.util.Log;

import com.winlator.star.container.Container;
import com.winlator.star.container.Shortcut;
import com.winlator.star.core.FileUtils;

import java.io.File;

/**
 * The library entries the Linux runtime owns. There is exactly one that is always there once the
 * runtime is installed — Valve's native arm64 Steam client — and it is a plain {@code .desktop}
 * entry like every other shortcut, so the Games tab, Big Picture and pinned shortcuts all pick it
 * up without knowing anything about gamescope.
 *
 * <p>The entry carries {@code runtime=gamescope} itself rather than relying on its container, so a
 * shortcut in an ordinary Wine container still launches into the Linux runtime.
 */
public final class LinuxShortcuts {
    private static final String TAG = "LinuxShortcuts";

    public static final String STEAM_NAME = "Steam (Linux)";
    private static final String STEAM_FILE = "Steam (Linux).desktop";
    /**
     * Exec is never run as written — the Linux runtime decides what to launch from the extras
     * below. It keeps the "wine " prefix every other shortcut has because {@link Shortcut} derives
     * its {@code path} by cutting at that prefix, and an entry without it gets a mangled path.
     */
    private static final String STEAM_EXEC = "wine linux:steam";

    private LinuxShortcuts() {}

    public static File steamShortcutFile(Container container) {
        return new File(container.getDesktopDir(), STEAM_FILE);
    }

    public static boolean hasSteamShortcut(Container container) {
        return steamShortcutFile(container).isFile();
    }

    /**
     * Writes the Steam entry into {@code container}, replacing any earlier copy. Returns false only
     * if the file could not be written.
     */
    /**
     * The tile the Games tab shows for the entry: Steam's own capsule for the client (app 753),
     * the same artwork the store serves for it. Written out of the app's resources the first time
     * the shortcut is created, because a cover art is read from a file path.
     */
    private static File coverArtFile(Context context) {
        return new File(context.getFilesDir(), "app_data/cover_arts/" + STEAM_NAME + ".png");
    }

    private static String writeCoverArt(Context context) {
        File out = coverArtFile(context);
        if (out.isFile()) return out.getPath();
        File dir = out.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) {
            Log.w(TAG, "cannot create " + dir);
            return "";
        }
        try (java.io.InputStream in = context.getResources().openRawResource(
                     com.winlator.star.R.drawable.steam_tile);
             java.io.OutputStream os = new java.io.FileOutputStream(out)) {
            byte[] buf = new byte[8192];
            for (int n; (n = in.read(buf)) > 0; ) os.write(buf, 0, n);
        } catch (Exception e) {
            Log.w(TAG, "could not write the tile for " + STEAM_NAME, e);
            return "";
        }
        return out.getPath();
    }

    public static boolean createSteamShortcut(Container container) {
        return createSteamShortcut(container, null);
    }

    public static boolean createSteamShortcut(Container container, Context context) {
        File desktopDir = container.getDesktopDir();
        if (!desktopDir.isDirectory() && !desktopDir.mkdirs()) {
            Log.w(TAG, "cannot create " + desktopDir);
            return false;
        }
        String content = "[Desktop Entry]\n"
                + "Name=" + STEAM_NAME + "\n"
                + "Exec=" + STEAM_EXEC + "\n"
                + "Type=Application\n"
                + "StartupWMClass=gamescope\n"
                + "\n"
                + "[Extra Data]\n"
                + Container.EXTRA_RUNTIME + "=" + Container.RUNTIME_GAMESCOPE + "\n"
                + LinuxRuntime.EXTRA_LINUX_MODE + "=" + LinuxRuntime.MODE_STEAM + "\n"
                // gamescope is a Wayland client; the launch path would pin this anyway.
                + "displayBackend=" + Container.DISPLAY_BACKEND_WAYLAND + "\n";
        if (context != null) {
            String cover = writeCoverArt(context);
            if (!cover.isEmpty()) content += "customCoverArtPath=" + cover + "\n";
        }
        boolean ok = FileUtils.writeString(steamShortcutFile(container), content);
        if (!ok) Log.w(TAG, "could not write " + steamShortcutFile(container));
        return ok;
    }

    /**
     * True for any entry the Linux runtime owns, whatever it is called. Matching on the runtime
     * extra rather than on {@link #STEAM_NAME} keeps entries written before the name settled -
     * and any the user made by hand - working like the one this class writes today.
     */
    public static boolean isLinuxEntry(Shortcut shortcut) {
        return shortcut != null
                && Container.RUNTIME_GAMESCOPE.equals(shortcut.getExtra(Container.EXTRA_RUNTIME));
    }

    /**
     * Gives a Linux entry the Steam tile if it has none yet. Entries written before the tile
     * existed carry no {@code customCoverArtPath}, and {@link Shortcut}'s name-based fallback is a
     * relative path that never resolves from an app process - so without this they keep the
     * generic placeholder for good. The bitmap is decoded here too: the path alone would only
     * show up on the load after next.
     */
    public static void ensureCoverArt(Context context, Shortcut shortcut) {
        if (context == null || !isLinuxEntry(shortcut)) return;
        String current = shortcut.getCustomCoverArtPath();
        if (current != null && !current.isEmpty() && new File(current).isFile()) return;
        String cover = writeCoverArt(context);
        if (cover.isEmpty()) return;
        shortcut.setCustomCoverArtPath(cover);
        shortcut.setCoverArt(BitmapFactory.decodeFile(cover));
    }

    public static boolean removeSteamShortcut(Container container) {
        File file = steamShortcutFile(container);
        return !file.isFile() || file.delete();
    }
}
