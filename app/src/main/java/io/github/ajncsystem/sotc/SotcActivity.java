// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.ajncsystem.sotc;

import android.os.Bundle;
import android.os.Environment;
import android.system.Os;
import android.util.Log;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * The game. Runs in its own process (":game"), started by LauncherActivity. SDL3 owns the
 * surface, input and the native main thread; this class turns the launcher's settings into the
 * game's environment (as SotC-PC's launcher does) and passes paths and the GPU driver as argv.
 */
public class SotcActivity extends SDLActivity {
    static final String TAG = "SotC";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The GPU library reads its environment in static constructors, when libmain.so loads
        // (inside super.onCreate): everything must be in place before that.
        Settings settings = new Settings(this);
        try {
            File ext = getExternalFilesDir(null);
            File user = new File(ext != null ? ext : getFilesDir(), "user");
            File gpuDir = new File(user, "gpu");
            //noinspection ResultOfMethodCallIgnored
            gpuDir.mkdirs();
            Os.setenv("BB_GPU_USER_DIR", gpuDir.getAbsolutePath(), true);
            Os.setenv("HOME", getFilesDir().getAbsolutePath(), false);
            for (Map.Entry<String, String> e : settings.gameEnvironment(this).entrySet()) {
                Os.setenv(e.getKey(), e.getValue(), true);
            }
            Log.i(TAG, "game environment: " + settings.gameEnvironment(this));
        } catch (Exception e) {
            Log.e(TAG, "cannot set the native environment", e);
        }
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // The game cannot be restarted inside the same process (guest state, FEX, Vulkan): end
        // the ":game" process so the next JOGAR starts clean.
        if (isFinishing()) System.exit(0);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "SDL3", "main"};
    }

    @Override
    protected String[] getArguments() {
        Settings settings = new Settings(this);
        List<String> args = new ArrayList<>();
        args.add("--native-lib-dir");
        args.add(getApplicationInfo().nativeLibraryDir);
        args.add("--files-dir");
        args.add(getFilesDir().getAbsolutePath());
        File ext = getExternalFilesDir(null);
        if (ext != null) {
            args.add("--external-dir");
            args.add(ext.getAbsolutePath());
        }

        File game = settings.str("game_dir").isEmpty() ? GameLocator.find(this) : new File(settings.str("game_dir"));
        if (game != null && new File(game, "eboot.bin").isFile()) {
            args.add("--app0");
            args.add(game.getAbsolutePath());
        } else {
            Log.w(TAG, "game dump not found (All files access: " + Environment.isExternalStorageManager() + ")");
        }

        DriverInstaller.Driver driver = null;
        try {
            driver = DriverInstaller.prepare(this, settings.str("driver"));
        } catch (Exception e) {
            Log.e(TAG, "GPU driver setup failed, falling back to the system driver", e);
        }
        if (driver != null) {
            args.add("--driver-dir");
            args.add(driver.dir);
            args.add("--driver-lib");
            args.add(driver.libraryName);
            args.add("--driver-name");
            args.add(driver.name);
        }
        Log.i(TAG, "native args: " + args);
        return args.toArray(new String[0]);
    }
}
