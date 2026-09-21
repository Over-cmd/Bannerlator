package com.winlator.star.linux;

import android.content.Context;
import android.util.Log;

import com.winlator.star.container.Container;
import com.winlator.star.container.ContainerManager;
import com.winlator.star.core.FileUtils;

import org.json.JSONObject;

import java.io.File;

/**
 * The Linux runtime's own settings, global and container-free.
 *
 * <p>A Linux session - gamescope and Valve's native Steam client - runs no Wine, yet until now its
 * entry had to live inside a Wine container, because a container is two things at once: a prefix
 * on disk and the envelope the launch reads its settings from. Only the second is wanted here.
 * This is that envelope on its own: a {@link Container} with a reserved id whose root is
 * {@code files/linux/}, no prefix beneath it, its config in {@code files/linux/.container} like any
 * other. {@link ContainerManager#getContainerById} hands it out for {@link #CONTAINER_ID}, so the
 * activity, the editors and the launch intents need no special case, and the Games tab lists what
 * is in its desktop directory beside every container's.
 *
 * <p>Global on purpose: there is one Steam client in the runtime and one set of settings for it,
 * which is how Steam itself thinks. Entries written into Wine containers before this existed keep
 * working - they carry the runtime themselves - and the runtime tab moves the Steam entry here.
 */
public final class LinuxSettings {
    private static final String TAG = "LinuxSettings";
    /** Never a real container's id: those count up from 1. */
    public static final int CONTAINER_ID = -7;
    public static final String DIR = "linux";
    public static final String NAME = "Linux runtime";

    private LinuxSettings() {}

    public static boolean isLinuxContainer(int id) {
        return id == CONTAINER_ID;
    }

    public static boolean isLinuxContainer(Container container) {
        return container != null && container.id == CONTAINER_ID;
    }

    public static File rootDir(Context context) {
        return new File(context.getFilesDir(), DIR);
    }

    /**
     * The settings container, read from disk or created with the Linux runtime's defaults the
     * first time. Cheap enough to build on demand; {@link ContainerManager} keeps one per instance.
     */
    public static Container container(Context context, ContainerManager manager) {
        File root = rootDir(context);
        //noinspection ResultOfMethodCallIgnored
        root.mkdirs();
        Container container = new Container(CONTAINER_ID, manager);
        container.setRootDir(root);
        JSONObject data = null;
        File config = container.getConfigFile();
        if (config.isFile()) {
            try {
                data = new JSONObject(FileUtils.readString(config));
            } catch (Exception e) {
                Log.w(TAG, "linux settings unreadable, starting over: " + e.getMessage());
            }
        }
        boolean fresh = data == null;
        if (fresh) data = new JSONObject();
        try {
            // The app's own defaults for every key the container reads, then the runtime's.
            Container.checkObsoleteOrMissingProperties(data);
            if (fresh) {
                data.put("name", NAME);
                // gamescope is a Wayland client, and its output is sized by this: the client's own
                // interface is the most expensive thing a session draws, so 720p to start.
                data.put("screenSize", Container.DEFAULT_SCREEN_SIZE);
                data.put("displayBackend", Container.DISPLAY_BACKEND_WAYLAND);
            }
            container.loadData(data);
            // Always the Linux runtime, whatever the stored data says.
            container.setRuntime(Container.RUNTIME_GAMESCOPE);
            container.setName(NAME);
            if (fresh) {
                container.saveData();
                Log.i(TAG, "created the Linux runtime's settings at " + config);
            }
        } catch (Exception e) {
            Log.e(TAG, "could not build the Linux settings container", e);
        }
        return container;
    }
}
