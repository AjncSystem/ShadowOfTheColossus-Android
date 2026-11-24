// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.ajncsystem.sotc;

import android.content.Context;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.util.Log;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * Finds the player's game dump (the folder with eboot.bin, CUSA08809 EU or CUSA08034 US).
 * Lookup order:
 *  1. {externalFilesDir}/game/game_dir.txt holding a folder path;
 *  2. {externalFilesDir}/game/CUSA08809 or CUSA08034;
 *  3. "PlayStation 4" folders on every storage volume (internal and SD cards), two levels deep,
 *     e.g. /storage/4A21-0000/PlayStation 4/Shadow Of The Colossus/CUSA08809.
 * Reading outside the app's own folder needs "All files access" (MANAGE_EXTERNAL_STORAGE).
 */
final class GameLocator {
    static final String[] SERIALS = {"CUSA08809", "CUSA08034"};

    private GameLocator() {}

    static File find(Context ctx) {
        File ext = ctx.getExternalFilesDir(null);
        if (ext != null) {
            File game = new File(ext, "game");
            File override = new File(game, "game_dir.txt");
            if (override.isFile()) {
                try {
                    String path = new String(Files.readAllBytes(override.toPath()), StandardCharsets.UTF_8).trim();
                    if (isGame(new File(path))) return new File(path);
                    Log.w(SotcActivity.TAG, "game_dir.txt names a folder without eboot.bin: " + path);
                } catch (Exception e) {
                    Log.w(SotcActivity.TAG, "cannot read game_dir.txt", e);
                }
            }
            for (String s : SERIALS) {
                File f = new File(game, s);
                if (isGame(f)) return f;
            }
        }
        // Apps cannot list /storage itself; the storage manager names every mounted volume.
        StorageManager sm = ctx.getSystemService(StorageManager.class);
        if (sm != null) {
            for (StorageVolume v : sm.getStorageVolumes()) {
                File root = v.getDirectory();
                if (root == null) continue;
                File found = search(new File(root, "PlayStation 4"), 2);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static File search(File dir, int depth) {
        if (depth < 0 || !dir.isDirectory()) return null;
        for (String s : SERIALS) {
            File f = new File(dir, s);
            if (isGame(f)) return f;
        }
        File[] kids = dir.listFiles(File::isDirectory);
        if (kids == null) return null;
        for (File k : kids) {
            File found = search(k, depth - 1);
            if (found != null) return found;
        }
        return null;
    }

    /** The dump inside a folder the player picked (the folder itself, or a CUSA folder up to two levels down). */
    static File findIn(File dir) {
        if (isGame(dir)) return dir;
        return search(dir, 2);
    }

    /** CUSA08809 / CUSA08034 from the folder name or sce_sys/param.sfo; "" when unknown. */
    static String titleId(File dir) {
        for (String s : SERIALS) if (dir.getName().startsWith(s)) return s;
        try {
            byte[] sfo = Files.readAllBytes(new File(dir, "sce_sys/param.sfo").toPath());
            String text = new String(sfo, StandardCharsets.ISO_8859_1);
            int i = text.indexOf("CUSA");
            if (i >= 0 && i + 9 <= text.length()) return text.substring(i, i + 9);
        } catch (Exception ignored) {
        }
        return "";
    }

    private static boolean isGame(File dir) {
        return new File(dir, "eboot.bin").isFile() && new File(dir, "sce_sys/param.sfo").isFile();
    }
}
