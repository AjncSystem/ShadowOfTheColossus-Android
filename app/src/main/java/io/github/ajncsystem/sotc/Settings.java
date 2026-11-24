// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.ajncsystem.sotc;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The launcher's settings and the game environment they turn into. Same keys, defaults and
 * mapping as SotC-PC's launcher (sotc_launcher_win.py: PLAY_MODE_VALUES, game_environment), minus
 * what Android has no use for (DLSS, FSR 3/4 frame generation, HDR, mouse, monitors), plus the
 * Android choices (GPU driver, Turnip debug flags, FEX memory ordering).
 */
final class Settings {
    static final String PREFS = "sotc_launcher";

    // Choice lists: {value, label}.
    static final String[][] PLAY_MODES = {
            {"normal", "Normal (PS4 puro)"}, {"pro", "Pro (PS4 Pro + qualidade)"},
            {"extreme", "Extremo (tudo no máximo)"}, {"custom", "Personalizado"}};
    static final String[][] FPS_MODES = {
            {"30", "30 FPS (como no PS4)"}, {"60", "60 FPS (modo 60 do jogo)"}, {"unlimited", "Ilimitado (acima de 60)"}};
    static final String[][] TEXTURES = {
            {"off", "Desligada"}, {"high", "Alta (estável)"}, {"max", "Máxima (nítida, experimental no modo Pro)"}};
    static final String[][] UPSCALERS = {{"fsr1", "FSR 1 (nítido)"}, {"bilinear", "Bilinear (simples)"}};
    static final String[][] SHARPNESS = {
            {"0", "Alta"}, {"250", "Média (padrão)"}, {"600", "Baixa"}, {"off", "Desligada"}};
    static final String[][] OUTPUTS = {
            {"1280x720", "1280 × 720 (720p)"}, {"1920x1080", "1920 × 1080 (1080p, tela do Odin 3)"}};
    static final String[][] VRAM_USE = {
            {"low", "Econômico"}, {"normal", "Padrão"}, {"high", "Alto"}};
    static final String[][] VRAM_PROFILES = {
            {"light", "Leve (recomendado no Android)"}, {"full", "Completo"}};
    static final String[][] PRESENT_MODES = {
            {"Fifo", "FIFO (VSync, padrão)"}, {"Mailbox", "Mailbox (baixa latência)"}, {"Immediate", "Immediate (pode cortar)"}};
    static final String[][] LANGUAGES = {
            {"", "Automático (o do jogo)"}, {"15", "Português (Brasil)"}, {"7", "Português (Portugal)"},
            {"1", "Inglês"}, {"3", "Espanhol"}, {"2", "Francês"}, {"4", "Alemão"}, {"5", "Italiano"}, {"0", "Japonês"}};
    static final String[][] AUDIO_CHANNELS = {{"2", "Estéreo (2)"}, {"6", "5.1 (6)"}, {"8", "7.1 (8)"}};
    static final String[][] READBACKS = {{"", "Precisas (padrão; o jogo precisa)"}, {"1", "Relaxadas (pode travar)"}};
    // Engine effects verified on SotC-PC (key, label, cvars when turned off).
    static final String[][] EFFECTS = {
            {"fx_vignette", "Vinheta (cantos escurecidos)", "r_vignetteOverrideEnable=1 r_vignetteEnable=0"},
            {"fx_fog", "Névoa atmosférica", "fog_enable=0"},
            {"fx_ssao", "Oclusão ambiente (SSAO)", "AO_Mode=0"},
            {"fx_motionblur", "Motion blur", "r_motionBlurEnable=0"}};

    private static final Map<String, Object> DEFAULTS = new LinkedHashMap<>();
    static {
        DEFAULTS.put("game_dir", "");
        DEFAULTS.put("prepared_dir", "");
        DEFAULTS.put("driver", "bundled:" + DriverInstaller.DEFAULT_PACKAGE);
        DEFAULTS.put("play_mode", "normal");
        DEFAULTS.put("fps_mode", "30");
        DEFAULTS.put("fps_dynamic", true);
        DEFAULTS.put("neo", false);
        DEFAULTS.put("textures", "high");
        DEFAULTS.put("af16", false);
        DEFAULTS.put("upscaler", "fsr1");
        DEFAULTS.put("sharpness", "250");
        DEFAULTS.put("output_res", "1920x1080");
        DEFAULTS.put("vram_use", "normal");
        DEFAULTS.put("vram_profile", "light");
        DEFAULTS.put("present_mode", "Fifo");
        DEFAULTS.put("show_fps", false);
        DEFAULTS.put("skip_logos", false);
        DEFAULTS.put("skip_film", false);
        for (String[] e : EFFECTS) DEFAULTS.put(e[0], true);
        DEFAULTS.put("perf_culling", true);
        DEFAULTS.put("perf_record_thread", true);
        DEFAULTS.put("perf_dma", true);
        DEFAULTS.put("rb_hot", true);
        DEFAULTS.put("volume", "100");
        DEFAULTS.put("audio_channels", "2");
        DEFAULTS.put("rumble", true);
        DEFAULTS.put("language", "");
        DEFAULTS.put("player_name", "");
        DEFAULTS.put("readbacks", "");
        DEFAULTS.put("engine_cvars", "");
        DEFAULTS.put("extra_env", "");
        DEFAULTS.put("debug_menu", false);
        // Android
        DEFAULTS.put("tu_debug", "");
        DEFAULTS.put("fex_tso", true);
        DEFAULTS.put("selftest", false);
    }

    private final SharedPreferences prefs;

    Settings(Context ctx) {
        prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    String str(String key) {
        Object d = DEFAULTS.get(key);
        return prefs.getString(key, d instanceof String ? (String) d : "");
    }

    boolean bool(String key) {
        Object d = DEFAULTS.get(key);
        return prefs.getBoolean(key, d instanceof Boolean && (Boolean) d);
    }

    void put(String key, String value) {
        prefs.edit().putString(key, value).apply();
        if (PLAY_MODE_KEYS.contains(key)) prefs.edit().putString("play_mode", "custom").apply();
    }

    void put(String key, boolean value) {
        prefs.edit().putBoolean(key, value).apply();
        if (PLAY_MODE_KEYS.contains(key)) prefs.edit().putString("play_mode", "custom").apply();
    }

    // Settings a play mode sets (changing one by hand makes the mode Custom, as on SotC-PC).
    private static final java.util.Set<String> PLAY_MODE_KEYS = new java.util.HashSet<>(java.util.Arrays.asList(
            "neo", "fps_mode", "textures", "af16", "upscaler", "sharpness", "vram_use"));

    /** Applies a play mode's values (SotC-PC PLAY_MODE_VALUES; Extreme without DLSS on Android). */
    void applyPlayMode(String mode) {
        SharedPreferences.Editor e = prefs.edit().putString("play_mode", mode);
        switch (mode) {
            case "normal":
                e.putBoolean("neo", false).putString("fps_mode", "30").putString("textures", "high")
                        .putBoolean("af16", false).putString("upscaler", "fsr1").putString("sharpness", "250")
                        .putString("vram_use", "normal");
                break;
            case "pro":
                e.putBoolean("neo", true).putString("fps_mode", "30").putString("textures", "high")
                        .putBoolean("af16", true).putString("vram_use", "high");
                break;
            case "extreme":
                e.putBoolean("neo", true).putString("fps_mode", "60").putString("textures", "max")
                        .putBoolean("af16", true).putString("vram_use", "high");
                break;
            default:
                break;
        }
        e.apply();
    }

    static String label(String[][] list, String value) {
        for (String[] item : list) if (item[0].equals(value)) return item[1];
        return value;
    }

    File preparedDir(Context ctx) {
        String p = str("prepared_dir");
        if (!p.isEmpty()) return new File(p);
        File ext = ctx.getExternalFilesDir(null);
        return new File(ext != null ? ext : ctx.getFilesDir(), "game");
    }

    /** The environment of the game process (SotC-PC game_environment, Android subset). */
    Map<String, String> gameEnvironment(Context ctx) {
        Map<String, String> env = new LinkedHashMap<>();
        env.put("SOTC_GAME_DIR", str("game_dir"));
        File prepared = preparedDir(ctx);
        env.put("SOTC_PREPARED_DIR", prepared.getAbsolutePath());
        env.put("SOTC_PORT_VERSION", BuildConfigLite.VERSION);

        String fps = str("fps_mode");
        StringBuilder modes = new StringBuilder();
        if (bool("af16")) modes.append("af16,");
        if (!fps.equals("30")) modes.append("fps60game,");
        modes.append("grassbuf");
        env.put("SOTC_MODES", modes.toString());
        env.put("SOTC_FPS_CAP", fps.equals("30") ? "30" : "off");
        env.put("SOTC_FPS_LIMIT", fps.equals("60") ? "60" : "0");
        if (fps.equals("60") && bool("fps_dynamic")) env.put("SOTC_FPS_DYNAMIC", dynamicFpsStep(displayHz(ctx)));
        env.put("BB_VBLANK_HZ", "120");
        env.put("BB_FULLSCREEN", "1");
        env.put("BB_PRESENT_MODE", str("present_mode"));
        if (!str("language").isEmpty()) env.put("BB_LANGUAGE", str("language"));
        if (!str("player_name").trim().isEmpty()) env.put("BB_USER_NAME", str("player_name").trim());
        if (bool("show_fps")) env.put("SOTC_FPS", "1");
        if (!str("readbacks").isEmpty()) env.put("BB_READBACKS", str("readbacks"));

        StringBuilder cvars = new StringBuilder();
        for (String[] e : EFFECTS) if (!bool(e[0])) cvars.append(e[2]).append(' ');
        if (bool("perf_culling")) cvars.append("doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0 ");
        if (bool("skip_logos")) cvars.append("skipSplash=1 ");
        if (bool("skip_film")) cvars.append("nointro ");
        cvars.append(str("engine_cvars"));
        env.put("SOTC_CVARS", cvars.toString().trim());
        env.put("SOTC_GRASS", "1 1 1 1 1");
        env.put("SOTC_RECORD_THREAD", bool("perf_record_thread") ? "1" : "0");
        if (!bool("perf_dma")) env.put("BB_DIRECT_MEMORY_ACCESS", "0");

        boolean light = str("vram_profile").equals("light");
        String vram = str("vram_use");
        if (light && (vram.equals("high") || vram.equals("max"))) vram = "normal";
        switch (vram) {
            case "low": env.put("SOTC_VRAM_GC_PERCENT", "50"); env.put("SOTC_ARENA_EVICT_MB", "1024"); break;
            case "high": env.put("SOTC_VRAM_GC_PERCENT", "75"); env.put("SOTC_ARENA_EVICT_MB", "4096"); break;
            default: env.put("SOTC_VRAM_GC_PERCENT", "60"); env.put("SOTC_ARENA_EVICT_MB", "1536"); break;
        }
        if (light) env.put("SOTC_STAGING_KEEP_VRAM_MB", "0");
        env.put("SOTC_VOLUME", str("volume"));
        env.put("SOTC_AUDIO_CHANNELS", str("audio_channels"));
        env.put("SOTC_SHADOW_REUSE", "0");
        env.put("SOTC_OUTPUT_RES", str("output_res"));
        env.put("SOTC_NEO", bool("neo") ? "1" : "0");
        env.put("SOTC_FRAMEGEN", "off");

        String textures = str("textures");
        if (!textures.equals("off")) {
            env.put("SOTC_LOD_LINGER", "1");
            env.put("SOTC_LOD_FEEDBACK", "1");
            env.put("SOTC_LOD_LEVEL", light ? (textures.equals("max") ? "1" : "2") : (textures.equals("max") ? "0" : "1"));
        }
        env.put("SOTC_POOL_FREEZE", "1");
        env.put("SOTC_RELEASE_DEFER", "1");
        env.put("SOTC_IMAGE_DEDICATED", "1");
        env.put("SOTC_UI_LANG", "pt");
        env.put("BB_FSR1", str("upscaler").equals("fsr1") ? "1" : "0");
        if (str("sharpness").equals("off")) {
            env.put("BB_RCAS", "0");
        } else {
            env.put("BB_RCAS", "1");
            env.put("BB_RCAS_ATTENUATION", str("sharpness"));
        }
        env.put("SOTC_MOUSE", "0");
        env.put("SOTC_RB_HOT", bool("rb_hot") ? "1" : "0");
        if (bool("debug_menu")) env.put("SOTC_DEBUG_MENU", "1");
        if (!bool("rumble")) env.put("SOTC_RUMBLE", "0");

        // Android
        if (!str("tu_debug").trim().isEmpty()) env.put("TU_DEBUG", str("tu_debug").trim());
        env.put("SOTC_FEX_TSO", bool("fex_tso") ? "1" : "0");
        if (bool("selftest")) env.put("SOTC_SELFTEST", "1");

        for (String item : str("extra_env").split("\\s+")) {
            item = item.trim();
            while (item.startsWith("=")) item = item.substring(1);
            int eq = item.indexOf('=');
            if (eq > 0) env.put(item.substring(0, eq), item.substring(eq + 1));
        }
        return env;
    }

    static float displayHz(Context ctx) {
        try {
            android.view.Display d = ctx.getDisplay();
            return d != null ? d.getRefreshRate() : 60f;
        } catch (Exception e) {
            return 60f;
        }
    }

    /** SotC-PC dynamic_fps_step: refresh / smallest whole divisor that brings it to 50 or less. */
    static String dynamicFpsStep(float hz) {
        if (hz < 30) hz = 60;
        int divisor = 2;
        while (hz / divisor > 50) divisor++;
        String s = String.format(java.util.Locale.ROOT, "%.2f", hz / divisor);
        s = s.replaceAll("0+$", "").replaceAll("\\.$", "");
        return s;
    }
}
