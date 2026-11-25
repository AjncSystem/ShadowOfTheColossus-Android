// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.ajncsystem.sotc;

import android.content.Context;
import android.os.Environment;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * GPU driver packages for libadrenotools (.zip with meta.json + the Vulkan .so, the format Eden,
 * Bachata S4 and the other emulators use). The launcher's choice (Settings "driver") is one of
 *   "system"                the device's own Qualcomm driver;
 *   "bundled:NAME.zip"      a package shipped in the APK's assets;
 *   "file:/path/NAME.zip"   any package on the device (Downloads, SD card...).
 * The chosen package is extracted into app-private storage, where the linker may dlopen it.
 */
final class DriverInstaller {
    static final String DEFAULT_PACKAGE = "Turnip_R3_barycentric_SotC.zip";

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

    /** A package the launcher can offer. */
    static final class Entry {
        final String value;        // Settings value
        final String title;        // meta.json name, or the file name
        final String detail;       // description / version / where it is
        Entry(String value, String title, String detail) {
            this.value = value;
            this.title = title;
            this.detail = detail;
        }
    }

    private DriverInstaller() {}

    // ---- discovery ----------------------------------------------------------------------------

    static List<Entry> discover(Context ctx) {
        List<Entry> out = new ArrayList<>();
        out.add(new Entry("system", "Driver do sistema (Qualcomm)", "o driver Adreno que vem no aparelho"));
        try {
            String[] assets = ctx.getAssets().list("");
            if (assets != null) {
                for (String a : assets) {
                    if (!a.toLowerCase().endsWith(".zip")) continue;
                    try (InputStream in = ctx.getAssets().open(a)) {
                        JSONObject meta = readMeta(in);
                        out.add(new Entry("bundled:" + a, title(meta, a), detail(meta, "embutido no app")));
                    }
                }
            }
        } catch (IOException e) {
            Log.w(SotcActivity.TAG, "cannot list bundled drivers", e);
        }
        List<File> roots = new ArrayList<>();
        File ext = ctx.getExternalFilesDir(null);
        if (ext != null) roots.add(new File(ext, "drivers"));
        roots.add(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS));
        for (File root : roots) scan(root, 3, out);
        return out;
    }

    private static void scan(File dir, int depth, List<Entry> out) {
        File[] kids = dir.listFiles();
        if (kids == null) return;
        for (File f : kids) {
            if (f.isDirectory()) {
                if (depth > 0) scan(f, depth - 1, out);
            } else if (f.getName().toLowerCase().endsWith(".zip") && f.length() < 256L << 20) {
                try (InputStream in = new FileInputStream(f)) {
                    JSONObject meta = readMeta(in);
                    if (meta == null) continue;  // not a driver package
                    out.add(new Entry("file:" + f.getAbsolutePath(), title(meta, f.getName()),
                                      detail(meta, f.getParentFile() != null ? f.getParentFile().getName() : "")));
                } catch (IOException ignored) {
                }
            }
        }
    }

    private static String title(JSONObject meta, String fallback) {
        return meta != null ? meta.optString("name", fallback) : fallback;
    }

    private static String detail(JSONObject meta, String where) {
        if (meta == null) return where;
        String d = meta.optString("description", "");
        String v = meta.optString("driverVersion", "");
        StringBuilder s = new StringBuilder();
        if (!d.isEmpty()) s.append(d);
        if (!v.isEmpty()) s.append(s.length() > 0 ? " · " : "").append(v);
        if (!where.isEmpty()) s.append(s.length() > 0 ? " · " : "").append(where);
        return s.toString();
    }

    /** meta.json of a driver package, or null when the zip has none. */
    static JSONObject readMeta(InputStream in) throws IOException {
        try (ZipInputStream zin = new ZipInputStream(in)) {
            ZipEntry e;
            while ((e = zin.getNextEntry()) != null) {
                if (new File(e.getName()).getName().equals("meta.json")) {
                    ByteArrayOutputStream buf = new ByteArrayOutputStream();
                    byte[] b = new byte[8192];
                    int n;
                    while ((n = zin.read(b)) > 0) buf.write(b, 0, n);
                    try {
                        return new JSONObject(buf.toString("UTF-8"));
                    } catch (Exception ex) {
                        return null;
                    }
                }
            }
        }
        return null;
    }

    // ---- install ------------------------------------------------------------------------------

    /** Extracts the chosen package; null for the system driver. */
    static Driver prepare(Context ctx, String choice) throws IOException {
        if (choice == null || choice.isEmpty()) choice = "bundled:" + DEFAULT_PACKAGE;
        if (choice.equals("system")) return null;

        final String sourceName;
        final long stampSize, stampTime;
        File file = null;
        String asset = null;
        if (choice.startsWith("file:")) {
            file = new File(choice.substring(5));
            if (!file.isFile()) throw new IOException("driver package missing: " + file);
            sourceName = file.getName();
            stampSize = file.length();
            stampTime = file.lastModified();
        } else {
            asset = choice.startsWith("bundled:") ? choice.substring(8) : choice;
            sourceName = asset;
            stampSize = -1;
            stampTime = apkTime(ctx);
        }

        String base = sourceName.endsWith(".zip") ? sourceName.substring(0, sourceName.length() - 4) : sourceName;
        File target = new File(new File(ctx.getFilesDir(), "drivers"), sanitize(base));
        File stamp = new File(target, ".stamp");
        String wantStamp = choice + "|" + stampSize + "|" + stampTime;
        if (!stamp.isFile() || !wantStamp.equals(readText(stamp))) {
            deleteTree(target);
            if (!target.mkdirs()) throw new IOException("cannot create " + target);
            try (InputStream in = file != null ? new FileInputStream(file) : ctx.getAssets().open(asset)) {
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

    /** Copies a package the user picked into the app's driver folder; returns its Settings value. */
    static String importPackage(Context ctx, InputStream in, String fileName) throws IOException {
        File ext = ctx.getExternalFilesDir(null);
        File dir = new File(ext != null ? ext : ctx.getFilesDir(), "drivers");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        File out = new File(dir, sanitize(fileName.toLowerCase().endsWith(".zip") ? fileName : fileName + ".zip"));
        try (OutputStream os = new FileOutputStream(out)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
        }
        try (InputStream check = new FileInputStream(out)) {
            if (readMeta(check) == null) {
                //noinspection ResultOfMethodCallIgnored
                out.delete();
                throw new IOException("o arquivo não é um pacote de driver (sem meta.json)");
            }
        }
        return "file:" + out.getAbsolutePath();
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
                File out = new File(target, new File(e.getName()).getName());
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
