// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.ajncsystem.sotc;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings.Secure;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

/**
 * The launcher: the Android counterpart of SotC-PC's launcher (pages Jogar, Jogo e saves, Imagem
 * e FPS, Controles e som, Driver de vídeo, Avançado). Settings go to the game process as its
 * environment (Settings.gameEnvironment), as sotc.ini / the PC launcher do. Made for a gamepad:
 * D-pad moves the focus, A confirms, B goes back.
 */
public class LauncherActivity extends Activity {
    private static final int REQ_GAME_DIR = 1, REQ_PREPARED_DIR = 2, REQ_DRIVER = 3;

    // Theme (dark, as SotC-PC's sotc_theme).
    private static final int BG = 0xFF101418, PANEL = 0xFF171C22, ITEM = 0xFF1F262E, FOCUS = 0xFF3A5A7A;
    private static final int TEXT = 0xFFE8E4DA, DIM = 0xFF9AA4AE, ACCENT = 0xFFD9B26A, OK = 0xFF7CC08A, BAD = 0xFFE07A6A;

    private Settings settings;
    private LinearLayout nav, content;
    private ScrollView contentScroll;
    private String page = "play";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        settings = new Settings(this);
        // First start: the game where GameLocator finds it.
        if (settings.str("game_dir").isEmpty()) {
            File g = GameLocator.find(this);
            if (g != null) settings.put("game_dir", g.getAbsolutePath());
        }
        buildFrame();
        showPage(page);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (content != null) showPage(page);  // back from the game or the permission screen
        hideSystemBars();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    private void hideSystemBars() {
        android.view.WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(android.view.WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    // ---- gamepad ------------------------------------------------------------------------------

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        if (code == KeyEvent.KEYCODE_BUTTON_A) {
            return super.dispatchKeyEvent(new KeyEvent(e.getDownTime(), e.getEventTime(), e.getAction(),
                    KeyEvent.KEYCODE_DPAD_CENTER, e.getRepeatCount(), e.getMetaState()));
        }
        if (code == KeyEvent.KEYCODE_BUTTON_B) {
            if (e.getAction() == KeyEvent.ACTION_UP) onBackPressed();
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    // ---- frame --------------------------------------------------------------------------------

    private void buildFrame() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.HORIZONTAL);
        root.setBackgroundColor(BG);

        LinearLayout side = new LinearLayout(this);
        side.setOrientation(LinearLayout.VERTICAL);
        side.setBackgroundColor(PANEL);
        side.setPadding(dp(16), dp(20), dp(16), dp(16));
        // The port's own art (the swordsman, as SotC-PC's window icon).
        android.widget.ImageView art = new android.widget.ImageView(this);
        art.setImageResource(R.drawable.launcher_art);
        art.setAdjustViewBounds(true);
        LinearLayout.LayoutParams artLp = new LinearLayout.LayoutParams(dp(96), dp(96));
        artLp.bottomMargin = dp(10);
        side.addView(art, artLp);
        TextView title = text("Shadow of the\nColossus", 22, ACCENT);
        title.setTypeface(Typeface.SERIF, Typeface.BOLD);
        side.addView(title);
        TextView sub = text("Android · v" + BuildConfigLite.VERSION, 13, DIM);
        sub.setPadding(0, dp(4), 0, dp(18));
        side.addView(sub);
        nav = new LinearLayout(this);
        nav.setOrientation(LinearLayout.VERTICAL);
        side.addView(nav);
        String[][] pages = {{"play", "Jogar"}, {"game", "Jogo e saves"}, {"image", "Imagem e FPS"},
                {"controls", "Controles e som"}, {"driver", "Driver de vídeo"}, {"advanced", "Avançado"}};
        for (String[] p : pages) {
            Button b = button(p[1], false);
            b.setOnClickListener(v -> showPage(p[0]));
            b.setTag(p[0]);
            nav.addView(b, matchWrap(0, dp(6)));
        }
        ScrollView sideScroll = new ScrollView(this);
        sideScroll.setBackgroundColor(PANEL);
        sideScroll.setFillViewport(true);
        sideScroll.addView(side);
        root.addView(sideScroll, new LinearLayout.LayoutParams(dp(260), ViewGroup.LayoutParams.MATCH_PARENT));

        contentScroll = new ScrollView(this);
        contentScroll.setFillViewport(true);
        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(28), dp(22), dp(28), dp(28));
        contentScroll.addView(content);
        root.addView(contentScroll, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));
        setContentView(root);
    }

    private void showPage(String name) {
        page = name;
        for (int i = 0; i < nav.getChildCount(); ++i) {
            View b = nav.getChildAt(i);
            b.setSelected(name.equals(b.getTag()));
        }
        content.removeAllViews();
        switch (name) {
            case "game": pageGame(); break;
            case "image": pageImage(); break;
            case "controls": pageControls(); break;
            case "driver": pageDriver(); break;
            case "advanced": pageAdvanced(); break;
            default: pagePlay(); break;
        }
        contentScroll.scrollTo(0, 0);
    }

    // ---- pages --------------------------------------------------------------------------------

    private void pagePlay() {
        heading("Jogar");
        File game = gameDir();
        File prepared = settings.preparedDir(this);
        boolean perm = Environment.isExternalStorageManager();
        boolean gameOk = game != null && new File(game, "eboot.bin").isFile();
        boolean prepOk = new File(prepared, "boot-linked.bin").isFile();

        status("Permissão de arquivos", perm, perm ? "acesso a todos os arquivos liberado" : "toque em Jogo e saves para liberar");
        status("Jogo", gameOk, gameOk ? gameDescription(game) : "escolha a pasta do jogo (CUSA08809 ou CUSA08034)");
        status("Imagem preparada", prepOk, prepOk ? prepared.getAbsolutePath()
                : "falta o boot-linked.bin do SotC-PC (Jogo e saves)");
        status("Driver de vídeo", true, driverTitle());

        choice("Modo de jogo", Settings.PLAY_MODES, settings.str("play_mode"), v -> {
            settings.applyPlayMode(v);
            showPage("play");
        });
        hint(playModeHint(settings.str("play_mode")));

        Button play = button("JOGAR", true);
        play.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26);
        play.setEnabled(perm && gameOk && prepOk);
        play.setOnClickListener(v -> startGame());
        LinearLayout.LayoutParams lp = matchWrap(dp(18), 0);
        lp.height = dp(84);
        content.addView(play, lp);
        play.requestFocus();

        Button log = button("Ver o relatório da última sessão", false);
        log.setOnClickListener(v -> showLastLog());
        content.addView(log, matchWrap(dp(12), 0));
    }

    private void pageGame() {
        heading("Jogo e saves");
        if (!Environment.isExternalStorageManager()) {
            hint("O jogo é lido de onde está (por exemplo \"PlayStation 4\" no cartão SD). O Android pede "
                    + "\"Acesso a todos os arquivos\" para isso.");
            action("Liberar acesso a todos os arquivos", () -> {
                try {
                    startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                            Uri.parse("package:" + getPackageName())));
                } catch (Exception e) {
                    startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                }
            });
        }
        File game = gameDir();
        label("Pasta do jogo");
        value(game != null ? game.getAbsolutePath() + "\n" + gameDescription(game) : "nenhuma");
        row(new String[]{"Escolher pasta…", "Procurar no aparelho"}, new Runnable[]{
                () -> pickFolder(REQ_GAME_DIR),
                () -> {
                    File g = GameLocator.find(this);
                    if (g != null) {
                        settings.put("game_dir", g.getAbsolutePath());
                        toast("Jogo encontrado: " + g.getAbsolutePath());
                    } else {
                        toast("Nenhum CUSA08809/CUSA08034 com eboot.bin em \"PlayStation 4\"");
                    }
                    showPage("game");
                }});
        hint("A atualização (pasta CUSA08809-patch ao lado) é aplicada por cima automaticamente.");

        File prepared = settings.preparedDir(this);
        label("Imagem preparada (pasta out do SotC-PC)");
        boolean prepOk = new File(prepared, "boot-linked.bin").isFile();
        value(prepared.getAbsolutePath() + "\n" + (prepOk ? "boot-linked.bin encontrado"
                + (new File(prepared, "content.bin").isFile() ? " · content.bin encontrado" : "") : "boot-linked.bin não encontrado"));
        row(new String[]{"Escolher pasta out…", "Usar a pasta do app"}, new Runnable[]{
                () -> pickFolder(REQ_PREPARED_DIR),
                () -> { settings.put("prepared_dir", ""); showPage("game"); }});
        hint("O SotC-PC gera a pasta out (boot-linked.bin, content.bin) a partir do seu dump na primeira vez "
                + "que o jogo abre no PC. Copie essa pasta para o Odin 3 e escolha-a aqui.");

        text("Nome do jogador", "player_name", false);
        choice("Idioma do jogo", Settings.LANGUAGES, settings.str("language"), v -> settings.put("language", v));
        File ext = getExternalFilesDir(null);
        hint("Saves: " + new File(ext, "user/savedata").getAbsolutePath());
    }

    private void pageImage() {
        heading("Imagem e FPS");
        choice("Resolução interna", Settings.RENDER_SIZES, settings.str("render_res"), v -> settings.put("render_res", v));
        choice("Saída", Settings.OUTPUTS, settings.str("output_res"), v -> settings.put("output_res", v));
        choice("Taxa de quadros", Settings.FPS_MODES, settings.str("fps_mode"), v -> { settings.put("fps_mode", v); showPage("image"); });
        if (settings.str("fps_mode").equals("60"))
            toggle("FPS dinâmico (cai para um passo estável quando 60 não dá)", "fps_dynamic");
        toggle("Modo PS4 Pro (Cinematográfico/Desempenho no menu do jogo)", "neo");
        choice("Qualidade das texturas", Settings.TEXTURES, settings.str("textures"), v -> settings.put("textures", v));
        toggle("Texturas nítidas (filtro anisotrópico 16x)", "af16");
        choice("Ampliação", Settings.UPSCALERS, settings.str("upscaler"), v -> settings.put("upscaler", v));
        choice("Nitidez (RCAS)", Settings.SHARPNESS, settings.str("sharpness"), v -> settings.put("sharpness", v));
        choice("Uso de memória de vídeo", Settings.VRAM_USE, settings.str("vram_use"), v -> settings.put("vram_use", v));
        choice("Perfil de memória", Settings.VRAM_PROFILES, settings.str("vram_profile"), v -> settings.put("vram_profile", v));
        choice("Apresentação", Settings.PRESENT_MODES, settings.str("present_mode"), v -> settings.put("present_mode", v));
        toggle("Barra de desempenho no topo (FPS, CPU, GPU, RAM, VRAM, bateria; L3 + R3 mostra/esconde)", "show_fps");
        label("Efeitos");
        for (String[] e : Settings.EFFECTS) toggle(e[1], e[0]);
        for (String[] e : Settings.PERF_EFFECTS) toggle(e[1] + " (pesa no FPS)", e[0]);
        toggle("Pular os logos de abertura", "skip_logos");
        toggle("Pular o filme de abertura e a tela de título", "skip_film");
    }

    private void pageControls() {
        heading("Controles e som");
        hint("O controle do Odin 3 é o controle do PS4: A = ✕, B = ○, X = □, Y = △, L1/R1, L2/R2, "
                + "Start = Options, Select = touchpad. Os analógicos são os do PS4.");
        toggle("Vibração", "rumble");
        String[][] vol = new String[11][2];
        for (int i = 0; i <= 10; ++i) { vol[i][0] = String.valueOf(i * 10); vol[i][1] = (i * 10) + "%"; }
        choice("Volume", vol, settings.str("volume"), v -> settings.put("volume", v));
        choice("Canais de som", Settings.AUDIO_CHANNELS, settings.str("audio_channels"), v -> settings.put("audio_channels", v));
    }

    private void pageDriver() {
        heading("Driver de vídeo");
        hint("Pacotes adrenotools (.zip com meta.json), os mesmos do Eden e do Bachata S4. Se o jogo fechar "
                + "com \"Vulkan device lost\", experimente outro Turnip.");
        String current = settings.str("driver");
        List<DriverInstaller.Entry> list = DriverInstaller.discover(this);
        for (DriverInstaller.Entry d : list) {
            boolean sel = d.value.equals(current);
            Button b = button((sel ? "● " : "○ ") + d.title + "\n" + d.detail, sel);
            b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            b.setAllCaps(false);
            b.setOnClickListener(v -> { settings.put("driver", d.value); showPage("driver"); });
            content.addView(b, matchWrap(dp(6), 0));
        }
        row(new String[]{"Importar driver (.zip)…", "Procurar de novo"}, new Runnable[]{
                () -> {
                    Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    i.setType("application/zip");
                    startActivityForResult(i, REQ_DRIVER);
                },
                () -> showPage("driver")});
        text("Opções do Turnip (TU_DEBUG, ex.: sysmem,nolrz)", "tu_debug", false);
    }

    private void pageAdvanced() {
        heading("Avançado");
        text("Variáveis do motor (cvars NOME=valor, separadas por espaço)", "engine_cvars", true);
        text("Variáveis de ambiente extras (NOME=valor)", "extra_env", true);
        choice("Leituras da GPU", Settings.READBACKS, settings.str("readbacks"), v -> settings.put("readbacks", v));
        label("Desempenho");
        toggle("Sem culling de índices na GPU (Pink-shadPS4)", "perf_culling");
        toggle("Gravação de comandos Vulkan em thread própria", "perf_record_thread");
        toggle("Acesso direto à memória (DMA)", "perf_dma");
        toggle("Leituras da GPU antecipadas (hot readbacks)", "rb_hot");
        label("Android");
        toggle("Ordem de memória do x86 (TSO) no FEX — desligar é mais rápido e pode travar", "fex_tso");
        toggle("Autoteste do FEX ao abrir", "selftest");
        toggle("Menu de depuração da Bluepoint", "debug_menu");
        action("Limpar o cache de shaders", () -> {
            File dir = new File(getExternalFilesDir(null), "user/gpu");
            deleteTree(dir);
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
            toast("Cache de shaders apagado; ele é refeito na próxima abertura.");
        });
        action("Ver o relatório da última sessão", this::showLastLog);
    }

    // ---- game ---------------------------------------------------------------------------------

    private void startGame() {
        Intent i = new Intent(this, SotcActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i);
    }

    private File gameDir() {
        String g = settings.str("game_dir");
        return g.isEmpty() ? null : new File(g);
    }

    private String gameDescription(File game) {
        String id = GameLocator.titleId(game);
        String region = "CUSA08809".equals(id) ? "Europa 1.01" : "CUSA08034".equals(id) ? "EUA 1.00" : "versão desconhecida";
        boolean patch = new File(game.getParentFile(), game.getName() + "-patch").isDirectory();
        return (id.isEmpty() ? "?" : id) + " · " + region + (patch ? " · com atualização" : "");
    }

    private String driverTitle() {
        String d = settings.str("driver");
        if (d.equals("system")) return "driver do sistema (Qualcomm)";
        for (DriverInstaller.Entry e : DriverInstaller.discover(this)) if (e.value.equals(d)) return e.title;
        return d.replaceFirst("^(bundled|file):", "");
    }

    private static String playModeHint(String mode) {
        switch (mode) {
            case "normal": return "Como no PS4: 30 FPS, texturas altas, grama e sombras do console, sem o modo Pro.";
            case "pro": return "Modo PS4 Pro a 30 FPS com texturas altas e filtro 16x.";
            case "extreme": return "Modo Pro a 60 FPS e texturas máximas. Pesado para o Odin 3.";
            default: return "Os ajustes das outras páginas ficam como você deixar.";
        }
    }

    private void showLastLog() {
        File ext = getExternalFilesDir(null);
        StringBuilder s = new StringBuilder();
        for (String name : new String[]{"m3-report.txt", "user/last_run.log"}) {
            File f = new File(ext, name);
            if (!f.isFile()) continue;
            try {
                String t = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
                if (t.length() > 12000) t = "…\n" + t.substring(t.length() - 12000);
                s.append("== ").append(name).append(" ==\n").append(t).append("\n\n");
            } catch (Exception ignored) {
            }
        }
        TextView tv = text(s.length() > 0 ? s.toString() : "Nenhuma sessão ainda.", 12, TEXT);
        tv.setTypeface(Typeface.MONOSPACE);
        tv.setTextIsSelectable(true);
        ScrollView sv = new ScrollView(this);
        sv.addView(tv);
        sv.setPadding(dp(16), dp(8), dp(16), dp(8));
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Última sessão").setView(sv).setPositiveButton("Fechar", null).show();
    }

    // ---- pickers ------------------------------------------------------------------------------

    private void pickFolder(int request) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(i, request);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        if (request == REQ_DRIVER) {
            String name = uri.getLastPathSegment() != null ? new File(uri.getLastPathSegment()).getName() : "driver.zip";
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                String value = DriverInstaller.importPackage(this, in, name);
                settings.put("driver", value);
                toast("Driver importado");
            } catch (Exception e) {
                toast("Não deu para importar: " + e.getMessage());
            }
            showPage("driver");
            return;
        }
        String path = treeUriToPath(uri);
        if (path == null) {
            toast("Escolha uma pasta do armazenamento interno ou do cartão SD");
            return;
        }
        if (request == REQ_GAME_DIR) {
            File f = new File(path);
            if (!new File(f, "eboot.bin").isFile()) {
                // The folder above the dump: take the CUSA folder inside it.
                File inside = GameLocator.findIn(f);
                if (inside != null) f = inside;
            }
            if (!new File(f, "eboot.bin").isFile()) toast("Essa pasta não tem eboot.bin");
            else settings.put("game_dir", f.getAbsolutePath());
            showPage("game");
        } else if (request == REQ_PREPARED_DIR) {
            if (!new File(path, "boot-linked.bin").isFile()) toast("Essa pasta não tem boot-linked.bin");
            settings.put("prepared_dir", path);
            showPage("game");
        }
    }

    /** content://…/tree/primary:Folder or …/tree/4A21-0000:Folder → /storage/… path. */
    static String treeUriToPath(Uri tree) {
        try {
            String id = DocumentsContract.getTreeDocumentId(tree);
            int colon = id.indexOf(':');
            if (colon < 0) return null;
            String volume = id.substring(0, colon), rel = id.substring(colon + 1);
            String base = volume.equalsIgnoreCase("primary") ? Environment.getExternalStorageDirectory().getAbsolutePath()
                    : "/storage/" + volume;
            return rel.isEmpty() ? base : base + "/" + rel;
        } catch (Exception e) {
            return null;
        }
    }

    // ---- widgets ------------------------------------------------------------------------------

    private interface Pick { void on(String value); }

    private void heading(String s) {
        TextView t = text(s, 28, ACCENT);
        t.setTypeface(Typeface.SERIF, Typeface.BOLD);
        t.setPadding(0, 0, 0, dp(12));
        content.addView(t);
    }

    private void label(String s) {
        TextView t = text(s, 17, TEXT);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(14), 0, dp(4));
        content.addView(t);
    }

    private void value(String s) {
        TextView t = text(s, 15, DIM);
        t.setPadding(0, 0, 0, dp(6));
        content.addView(t);
    }

    private void hint(String s) {
        TextView t = text(s, 14, DIM);
        t.setPadding(0, dp(4), 0, dp(4));
        content.addView(t);
    }

    private void status(String what, boolean ok, String detail) {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setPadding(0, dp(4), 0, dp(4));
        TextView mark = text(ok ? "✔" : "✘", 18, ok ? OK : BAD);
        mark.setPadding(0, 0, dp(10), 0);
        l.addView(mark);
        TextView t = text(what + ": " + detail, 16, TEXT);
        l.addView(t);
        content.addView(l);
    }

    private void choice(String title, String[][] options, String current, Pick pick) {
        Button b = button(title + ":  " + Settings.label(options, current), false);
        b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        b.setAllCaps(false);
        b.setOnClickListener(v -> {
            String[] labels = new String[options.length];
            int sel = -1;
            for (int i = 0; i < options.length; ++i) {
                labels[i] = options[i][1];
                if (options[i][0].equals(current)) sel = i;
            }
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(title)
                    .setSingleChoiceItems(labels, sel, (d, which) -> {
                        d.dismiss();
                        pick.on(options[which][0]);
                        showPage(page);
                    }).show();
        });
        content.addView(b, matchWrap(dp(6), 0));
    }

    private void toggle(String title, String key) {
        Switch s = new Switch(this);
        s.setText(title);
        s.setTextColor(TEXT);
        s.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        s.setChecked(settings.bool(key));
        s.setPadding(dp(14), dp(10), dp(14), dp(10));
        s.setBackground(itemBackground(false));
        s.setOnCheckedChangeListener((CompoundButton b, boolean on) -> settings.put(key, on));
        content.addView(s, matchWrap(dp(6), 0));
    }

    private void text(String title, String key, boolean multiline) {
        label(title);
        EditText e = new EditText(this);
        e.setText(settings.str(key));
        e.setTextColor(TEXT);
        e.setHintTextColor(DIM);
        e.setBackground(itemBackground(false));
        e.setPadding(dp(12), dp(10), dp(12), dp(10));
        e.setInputType(InputType.TYPE_CLASS_TEXT | (multiline ? InputType.TYPE_TEXT_FLAG_MULTI_LINE : 0)
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        e.setOnFocusChangeListener((v, has) -> { if (!has) settings.put(key, e.getText().toString().trim()); });
        content.addView(e, matchWrap(0, 0));
    }

    private void action(String title, Runnable r) {
        Button b = button(title, false);
        b.setOnClickListener(v -> r.run());
        content.addView(b, matchWrap(dp(8), 0));
    }

    private void row(String[] titles, Runnable[] actions) {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        for (int i = 0; i < titles.length; ++i) {
            Button b = button(titles[i], false);
            final Runnable r = actions[i];
            b.setOnClickListener(v -> r.run());
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            if (i > 0) lp.leftMargin = dp(8);
            l.addView(b, lp);
        }
        content.addView(l, matchWrap(dp(6), 0));
    }

    private TextView text(String s, int sp, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        return t;
    }

    private Button button(String s, boolean primary) {
        Button b = new Button(this);
        b.setText(s);
        b.setTextColor(primary ? 0xFF101418 : TEXT);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        b.setAllCaps(primary);
        b.setPadding(dp(16), dp(12), dp(16), dp(12));
        b.setBackground(itemBackground(primary));
        b.setFocusable(true);
        return b;
    }

    private StateListDrawable itemBackground(boolean primary) {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[]{android.R.attr.state_focused}, rounded(FOCUS, ACCENT));
        s.addState(new int[]{android.R.attr.state_pressed}, rounded(FOCUS, ACCENT));
        s.addState(new int[]{android.R.attr.state_selected}, rounded(primary ? ACCENT : 0xFF2C3E50, ACCENT));
        s.addState(new int[]{-android.R.attr.state_enabled}, rounded(0xFF2A2F36, 0));
        s.addState(new int[]{}, rounded(primary ? ACCENT : ITEM, 0));
        return s;
    }

    private GradientDrawable rounded(int fill, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(dp(8));
        if (stroke != 0) g.setStroke(dp(2), stroke);
        return g;
    }

    private LinearLayout.LayoutParams matchWrap(int top, int bottom) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = top;
        lp.bottomMargin = bottom;
        return lp;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_LONG).show();
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
