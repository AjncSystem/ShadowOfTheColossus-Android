// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.ajncsystem.sotc;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.system.Os;
import android.util.Log;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.List;

/**
 * Entry activity. SDL3 owns the surface, input and the native main thread; this class only
 * prepares what the native side needs before SDL_main runs (GPU driver, paths) and passes it as
 * argv.
 */
public class SotcActivity extends SDLActivity {
    static final String TAG = "SotC";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The GPU library computes its cache folders in static constructors, when libmain.so
        // loads (inside super.onCreate): its environment must be in place before that.
        try {
            java.io.File ext = getExternalFilesDir(null);
            java.io.File gpuDir = new java.io.File(ext != null ? ext : getFilesDir(), "user/gpu");
            //noinspection ResultOfMethodCallIgnored
            gpuDir.mkdirs();
            Os.setenv("BB_GPU_USER_DIR", gpuDir.getAbsolutePath(), true);
            Os.setenv("HOME", getFilesDir().getAbsolutePath(), false);
        } catch (Exception e) {
            Log.e(TAG, "cannot set the native environment", e);
        }
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // The game dump usually lives outside the app's folder (e.g. "PlayStation 4" on the SD
        // card): ask once for "All files access". The game starts on the next launch after it
        // is allowed.
        if (!Environment.isExternalStorageManager()) {
            Log.i(TAG, "asking for All files access to read the game dump");
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                         Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "SDL3", "main"};
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        args.add("--native-lib-dir");
        args.add(getApplicationInfo().nativeLibraryDir);
        args.add("--files-dir");
        args.add(getFilesDir().getAbsolutePath());
        java.io.File ext = getExternalFilesDir(null);
        if (ext != null) {
            args.add("--external-dir");
            args.add(ext.getAbsolutePath());
        }

        java.io.File game = GameLocator.find(this);
        if (game != null) {
            args.add("--app0");
            args.add(game.getAbsolutePath());
        } else {
            Log.w(TAG, "game dump not found (All files access: " + Environment.isExternalStorageManager() + ")");
        }

        DriverInstaller.Driver driver = null;
        try {
            driver = DriverInstaller.prepare(this);
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
