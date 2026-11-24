// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.ajncsystem.sotc;

import android.content.Context;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Installs an adrenotools driver package (.zip with meta.json + the Vulkan .so) into app-private
 * storage, where the linker is allowed to dlopen it.
 *
 * Package lookup, first match wins:
 *  1. {externalFilesDir}/drivers/selected.txt naming a zip in that folder;
 *  2. {externalFilesDir}/drivers/Turnip_Gen8_V38.zip;
 *  3. the newest zip in {externalFilesDir}/drivers;
 *  4. assets/Turnip_Gen8_V38.zip bundled in the APK.
 * {externalFilesDir} is /sdcard/Android/data/io.github.ajncsystem.sotc/files and can be written
 * with `adb push`. A file named "system" in selected.txt keeps the system (Qualcomm) driver.
 */
final class DriverInstaller {
    static final String DEFAULT_PACKAGE = "Turnip_Gen8_V38.zip";

    static final class Driver {
        final String name;
        final String dir;          // with trailing slash, as adrenotools expects
        final String libraryName;

        Driver(String name, String dir, String libraryName) {
            this.name = name;
            this.dir = dir;
            this.libraryName = libraryName;
        }
    }

    private DriverInstaller() {}

    static Driver prepare(Context ctx) throws IOException {
        File extDrivers = null;
        File ext = ctx.getExternalFilesDir(null);
        if (ext != null) {
            extDrivers = new File(ext, "drivers");
            //noinspection ResultOfMethodCallIgnored
            extDrivers.mkdirs();
        }

        String selected = null;
        if (extDrivers != null) {
            File sel = new File(extDrivers, "selected.txt");
            if (sel.isFile()) {
                selected = new String(Files.readAllBytes(sel.toPath()), StandardCharsets.UTF_8).trim();
            }
        }
        if ("system".equalsIgnoreCase(selected)) {
            Log.i(SotcActivity.TAG, "selected.txt asks for the system driver");
            return null;
        }

        File source = pickExternal(extDrivers, selected);
        String sourceName;
        long stampSize;
        long stampTime;
        if (source != null) {
            sourceName = source.getName();
            stampSize = source.length();
            stampTime = source.lastModified();
        } else {
            sourceName = DEFAULT_PACKAGE;
            stampSize = -1;
            stampTime = ctx.getPackageManager() != null ? apkTime(ctx) : 0;
        }

        String base = sourceName.endsWith(".zip") ? sourceName.substring(0, sourceName.length() - 4) : sourceName;
        File target = new File(new File(ctx.getFilesDir(), "drivers"), sanitize(base));
        File stamp = new File(target, ".stamp");
        String wantStamp = sourceName + "|" + stampSize + "|" + stampTime;

        if (!stamp.isFile() || !wantStamp.equals(readText(stamp))) {
            deleteTree(target);
            if (!target.mkdirs()) throw new IOException("cannot create " + target);
            try (InputStream in = source != null ? new FileInputStream(source) : ctx.getAssets().open(DEFAULT_PACKAGE)) {
                unzip(in, target);
            }
            writeText(stamp, wantStamp);
            Log.i(SotcActivity.TAG, "installed GPU driver " + sourceName + " into " + target);
        }

        File meta = new File(target, "meta.json");
        String libraryName = "libvulkan_freedreno.so";
        String name = base;
        if (meta.isFile()) {
            try {
                JSONObject json = new JSONObject(readText(meta));
                libraryName = json.optString("libraryName", libraryName);
                name = json.optString("name", name) + " (" + json.optString("description", "") + ")";
            } catch (Exception e) {
                Log.w(SotcActivity.TAG, "bad meta.json in " + sourceName, e);
            }
        }
        File lib = new File(target, libraryName);
        if (!lib.isFile()) throw new IOException(libraryName + " missing from " + sourceName);
        //noinspection ResultOfMethodCallIgnored
        lib.setReadOnly(); // Android 14+ refuses to dlopen writable files from app storage
        return new Driver(name, target.getAbsolutePath() + "/", libraryName);
    }

    private static File pickExternal(File dir, String selected) {
        if (dir == null) return null;
        if (selected != null && !selected.isEmpty()) {
            File f = new File(dir, selected);
            if (f.isFile()) return f;
            Log.w(SotcActivity.TAG, "selected.txt names a missing file: " + selected);
        }
        File def = new File(dir, DEFAULT_PACKAGE);
        if (def.isFile()) return def;
        File[] zips = dir.listFiles((d, n) -> n.toLowerCase().endsWith(".zip"));
        if (zips == null || zips.length == 0) return null;
        Arrays.sort(zips, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        return zips[0];
    }

    private static long apkTime(Context ctx) {
        try {
            return ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).lastUpdateTime;
        } catch (Exception e) {
            return 0;
        }
    }

    private static String sanitize(String s) {
        return s.replaceAll("[^A-Za-z0-9._-]", "_");
    }

    private static void unzip(InputStream in, File target) throws IOException {
        String root = target.getCanonicalPath() + File.separator;
        try (ZipInputStream zin = new ZipInputStream(in)) {
            ZipEntry e;
            byte[] buf = new byte[1 << 16];
            while ((e = zin.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                // Flatten: adrenotools wants meta.json and the library side by side.
                String name = new File(e.getName()).getName();
                File out = new File(target, name);
                if (!out.getCanonicalPath().startsWith(root)) throw new IOException("bad zip entry " + e.getName());
                try (OutputStream os = new FileOutputStream(out)) {
                    int n;
                    while ((n = zin.read(buf)) > 0) os.write(buf, 0, n);
                }
            }
        }
    }

    private static String readText(File f) {
        try {
            return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File f, String s) throws IOException {
        Files.write(f.toPath(), s.getBytes(StandardCharsets.UTF_8));
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
