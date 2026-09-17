package com.winlator.star.linux;

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
    public static boolean createSteamShortcut(Container container) {
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
        boolean ok = FileUtils.writeString(steamShortcutFile(container), content);
        if (!ok) Log.w(TAG, "could not write " + steamShortcutFile(container));
        return ok;
    }

    public static boolean removeSteamShortcut(Container container) {
        File file = steamShortcutFile(container);
        return !file.isFile() || file.delete();
    }
}
