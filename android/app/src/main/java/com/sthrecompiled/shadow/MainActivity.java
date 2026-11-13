package com.sthrecompiled.shadow;

import android.Manifest;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.Settings;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;

// SDL's activity running libmain.so. The game goes in the Shadow folder on the phone's storage
// (/storage/emulated/0/Shadow) or at the top of an SD card, as its own files (default.xbe and the rest) or as a disc
// image (.iso). Reading it needs all files access, a special access Android grants on a settings page of its own,
// never among the app's ordinary permissions. This asks for it and says where the game goes, each time the app comes
// back without them; the game itself waits and starts as soon as it finds the files.
public class MainActivity extends SDLActivity {
    static final String SHARED = "Shadow";

    private AlertDialog dialog;   // the one up, at most one at a time
    private boolean notNow;       // the player turned the access down this run
    private boolean imported;     // a copy this run finished: the game looks for it by itself
    private boolean importing;    // a copy is running: no other dialog may come over its progress
    private final GameImporter importer = new GameImporter(this);

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        File own = getExternalFilesDir(null);
        if (own != null) new File(own, "assets").mkdirs();
        hideSystemBars();
    }

    // A controller some phones' game services pass on as a virtual device (MIUI's took a DualShock 4 on a Redmi Note
    // 8 Pro): SDL sees no controller in it and drops its input. Its buttons and sticks go to the game as port 0's
    // through here (native/input.cpp); a controller SDL has goes its own way as before.
    private static boolean loose(InputEvent e, int sources) {
        return !mBrokenLibraries && (e.getSource() & sources) != 0
                && !SDLControllerManager.isDeviceSDLJoystick(e.getDeviceId());
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        boolean pad = KeyEvent.isGamepadButton(e.getKeyCode()) || (e.getKeyCode() >= KeyEvent.KEYCODE_DPAD_UP
                && e.getKeyCode() <= KeyEvent.KEYCODE_DPAD_RIGHT);
        if (pad && loose(e, InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK)) {
            if (e.getRepeatCount() == 0) NativeBridge.looseKey(e.getKeyCode(), e.getAction() == KeyEvent.ACTION_DOWN);
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (loose(e, InputDevice.SOURCE_JOYSTICK) && e.getActionMasked() == MotionEvent.ACTION_MOVE) {
            // the right stick on Z and RZ, the triggers on LTRIGGER and RTRIGGER or BRAKE and GAS, as Android has them
            NativeBridge.looseAxes(e.getAxisValue(MotionEvent.AXIS_X), e.getAxisValue(MotionEvent.AXIS_Y),
                    e.getAxisValue(MotionEvent.AXIS_Z), e.getAxisValue(MotionEvent.AXIS_RZ),
                    Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE)),
                    Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS)),
                    e.getAxisValue(MotionEvent.AXIS_HAT_X), e.getAxisValue(MotionEvent.AXIS_HAT_Y));
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!aboutSeen()) {
            showAbout(true);
            return;
        }
        askForGame();
    }

    // SDL's older immersive flags don't always come back after a dialog (MIUI on Android 11 kept the status bar):
    // the bars are hidden again each time the game has the focus, a swipe shows them for a moment
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
        else if (!mBrokenLibraries) NativeBridge.looseRelease();  // its ups go elsewhere while the game is not in front
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT < 30) {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
            return;
        }
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    // EXIT TO LAUNCHER in the port menu, from the game's thread: the activity goes away and the phone's launcher is
    // in front again. The game process ends with it (native/main.cpp sees the window go).
    public void finishGame() {
        runOnUiThread(() -> {
            setResult(RESULT_OK);
            finishAndRemoveTask();
        });
    }

    // a short buzz for the touch controls' presses, from the game's thread
    public void tapHaptic(int ms) {
        Vibrator v = (Vibrator) getSystemService(VIBRATOR_SERVICE);
        if (v == null || !v.hasVibrator()) return;
        try {
            v.vibrate(VibrationEffect.createOneShot(Math.max(1, ms), VibrationEffect.DEFAULT_AMPLITUDE));
        } catch (Exception e) {
            // a phone without control of its vibration motor: nothing to do
        }
    }

    // ---- about the port
    //
    // Who made it and where it came from, in English and in Russian: once at the first launch, and from the menu's
    // ABOUT page whenever it is wanted. Two buttons open the channel and the port it was modelled on.

    private static final String TELEGRAM = "https://t.me/player1444ports";
    private static final String BLACK_ANDROID = "https://github.com/RanggaCasper/Black-Android";
    private static final String PREFS = "shadow";
    private static final String SEEN_ABOUT = "about_seen";
    private static final String ABOUT_EN =
            "Shadow the Hedgehog for Android, ported by Player1444.\n"
            + "Telegram channel: " + TELEGRAM + "\n\n"
            + "Ported with Rangga Casper's Black Android port as the reference: " + BLACK_ANDROID + "\n\n"
            + "No game files and no game code are in this app: it is built from your own disc image. Shadow the "
            + "Hedgehog is a trademark of SEGA; this project is not affiliated with SEGA.";
    private static final String ABOUT_RU =
            "Shadow the Hedgehog для Android, порт сделал Player1444.\n"
            + "Телеграм-канал: " + TELEGRAM + "\n\n"
            + "За основу портирования взят порт Black for Android (Rangga Casper): " + BLACK_ANDROID + "\n\n"
            + "В приложении нет ни файлов игры, ни её кода — сборка делается из вашего собственного образа диска. "
            + "Shadow the Hedgehog — торговая марка SEGA; проект с SEGA не связан.";

    private boolean aboutSeen() {
        return getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(SEEN_ABOUT, false);
    }

    // from the port menu, on the game's thread
    public void showAbout() {
        showAbout(false);
    }

    // findGame: the game still has to be looked for once the dialog goes (the first launch), not while it is running
    private void showAbout(boolean findGame) {
        runOnUiThread(() -> {
            if (isFinishing()) return;
            if (dialog != null && dialog.isShowing()) dialog.dismiss();
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(SEEN_ABOUT, true).apply();

            TextView body = new TextView(this);
            body.setText(ABOUT_EN + "\n\n---\n\n" + ABOUT_RU);
            body.setTextIsSelectable(true);  // the links can be copied out
            int pad = Math.round(20 * getResources().getDisplayMetrics().density);
            body.setPadding(pad, pad / 2, pad, 0);
            ScrollView scroll = new ScrollView(this);
            scroll.addView(body);

            dialog = new AlertDialog.Builder(this)
                    .setTitle("About this port  /  Об этом порте")
                    .setView(scroll)
                    .setPositiveButton("Telegram", (d, w) -> openUrl(TELEGRAM))
                    .setNeutralButton("Black Android", (d, w) -> openUrl(BLACK_ANDROID))
                    .setNegativeButton("Close", null)
                    .setOnDismissListener(d -> {
                        dialog = null;
                        if (findGame) getWindow().getDecorView().postDelayed(this::askForGame, 120);
                    })
                    .show();
        });
    }

    private void openUrl(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (Exception e) {
            toast("No app can open " + url);
        }
    }

    // ---- the log
    //
    // A report is worth nothing without the log, and finding it means walking into Android/data. The newest log's tail
    // is a tap away instead: COPY LOG puts it on the clipboard, SEND LOG hands it to another app. Both are called from
    // the game's thread (the port menu) and from the launcher's own dialog, so they hop to the UI thread themselves.

    private static final int LOG_LINES = 3000;      // the tail that is taken
    private static final int LOG_MAX_CHARS = 220000;  // a share may not outgrow a binder transaction

    // the newest .log/.txt in logs/, beside the app's files (internal storage when the phone gave no external one)
    private File newestLog() {
        File best = null;
        for (File base : new File[] {getExternalFilesDir(null), getFilesDir()}) {
            if (base == null) continue;
            File[] files = new File(base, "logs").listFiles();
            if (files == null) continue;
            for (File f : files) {
                String n = f.getName().toLowerCase();
                if (!f.isFile() || !(n.endsWith(".log") || n.endsWith(".txt"))) continue;
                if (best == null || f.lastModified() > best.lastModified()) best = f;
            }
        }
        return best;
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    private String logTail() {
        File f = newestLog();
        if (f == null) return "# no log yet: the game has not started on this phone";
        ArrayDeque<String> tail = new ArrayDeque<>();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String line; (line = r.readLine()) != null; ) {
                tail.addLast(line);
                if (tail.size() > LOG_LINES) tail.removeFirst();
            }
        } catch (Exception e) {
            return "# could not read " + f.getName() + ": " + e.getMessage();
        }
        StringBuilder b = new StringBuilder();
        b.append("# Shadow the Hedgehog ").append(versionName()).append(", ").append(Build.MANUFACTURER).append(' ')
                .append(Build.MODEL).append(", Android ").append(Build.VERSION.RELEASE).append(" (API ")
                .append(Build.VERSION.SDK_INT).append(")\n");
        b.append("# the last ").append(tail.size()).append(" lines of ").append(f.getPath()).append('\n');
        for (String line : tail) b.append(line).append('\n');
        String text = b.toString();
        return text.length() > LOG_MAX_CHARS ? text.substring(text.length() - LOG_MAX_CHARS) : text;
    }

    public void copyLog() {
        runOnUiThread(() -> {
            String text = logTail();
            ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            if (cm == null) {
                toast("No clipboard on this phone");
                return;
            }
            cm.setPrimaryClip(ClipData.newPlainText("Shadow log", text));
            toast("Log copied: " + text.length() / 1024 + " KB");
        });
    }

    public void shareLog() {
        runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_SEND);
            i.setType("text/plain");
            i.putExtra(Intent.EXTRA_SUBJECT, "Shadow the Hedgehog log");
            i.putExtra(Intent.EXTRA_TEXT, logTail());
            try {
                startActivity(Intent.createChooser(i, "Send the log"));
            } catch (Exception e) {
                copyLog();  // no share sheet: the clipboard is still better than nothing
            }
        });
    }

    private void toast(String text) {
        Toast.makeText(this, text, Toast.LENGTH_SHORT).show();
    }

    // ---- where the game goes

    // Android 11 on has all files access, a page of its own in the settings; Android 10 the storage permission, with
    // the legacy storage the manifest asks for
    private static final boolean ALL_FILES_ACCESS = Build.VERSION.SDK_INT >= 30;
    private static final int STORAGE_REQUEST = 0x426C;
    private boolean storageAsked;  // the permission was asked for this run

    private boolean hasStorageAccess() {
        if (ALL_FILES_ACCESS) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    // the game's files, as native/game_files.cpp looks for them: default.xbe beside the folder's assets, or any .iso
    private static boolean hasGame(File dir) {
        if (dir == null || !dir.isDirectory()) return false;
        File[] files = dir.listFiles();
        if (files == null) return false;
        for (File f : files) {
            String n = f.getName().toLowerCase();
            if (f.isFile() && (n.equals("default.xbe") || n.endsWith(".iso"))) return true;
        }
        return false;
    }

    private static boolean hasGameAnywhere(File dir) {
        return hasGame(dir) || hasGame(new File(dir, "assets"));
    }

    private void askForGame() {
        // an import in progress owns the screen: its own dialog says what is happening, and the game may be looking at
        // a half written image
        if (importing || imported || (dialog != null && dialog.isShowing())) return;
        File own = getExternalFilesDir(null);
        if (own != null && hasGameAnywhere(new File(own, "assets"))) return;
        if (!hasStorageAccess()) {
            if (!notNow) askAllFilesAccess();
            return;
        }
        File shared = new File(Environment.getExternalStorageDirectory(), SHARED);
        shared.mkdirs();
        if (hasGameAnywhere(shared)) return;
        for (File card : cardFolders())
            if (hasGameAnywhere(card)) return;
        tellWhereTheGameGoes();
    }

    // the Shadow folder on each SD card or USB stick mounted, as native/game_files.cpp looks in them
    private List<File> cardFolders() {
        List<File> out = new ArrayList<>();
        if (!ALL_FILES_ACCESS) {
            // Android 10: each card's root above the app's own folder on it, <root>/Android/data/<package>/files
            File[] own = getExternalFilesDirs(null);
            for (int i = 1; i < own.length; i++) {
                File root = own[i];
                for (int up = 0; root != null && up < 4; up++) root = root.getParentFile();
                if (root != null) out.add(new File(root, SHARED));
            }
            return out;
        }
        StorageManager sm = getSystemService(StorageManager.class);
        if (sm == null) return out;
        for (StorageVolume v : sm.getStorageVolumes()) {
            File root = v.getDirectory();
            if (v.isPrimary() || root == null || !Environment.MEDIA_MOUNTED.equals(v.getState())) continue;
            out.add(new File(root, SHARED));
        }
        return out;
    }

    private List<File> ownFolders() {
        List<File> out = new ArrayList<>();
        for (File dir : getExternalFilesDirs(null)) {
            if (dir != null) out.add(new File(dir, "assets"));
        }
        return out;
    }

    // the same folders for native/game_files.cpp, from the game's thread: some phones (a Sony on Android 11) don't let
    // the app list /storage, though each card's folder opens
    public String[] cardFolderPaths() {
        List<File> cards = cardFolders();
        String[] out = new String[cards.size()];
        for (int i = 0; i < out.length; i++) out[i] = cards.get(i).getPath();
        return out;
    }

    public String[] ownFolderPaths() {
        List<File> own = ownFolders();
        String[] out = new String[own.size()];
        for (int i = 0; i < out.length; i++) out[i] = own.get(i).getPath();
        return out;
    }

    private void askAllFilesAccess() {
        storageAsked = true;
        if (!ALL_FILES_ACCESS) {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE}, STORAGE_REQUEST);
            return;
        }
        show("One permission first", "Shadow the Hedgehog reads the game from a folder of yours, which Android only "
                + "allows with all files access. It is the port's own, never the whole storage's contents: only the "
                + "Shadow folder is opened.\n\nOn the settings page that opens, allow access to files and media.",
                "Open settings", () -> {
                    try {
                        Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                        i.setData(Uri.parse("package:" + getPackageName()));
                        startActivity(i);
                    } catch (Exception e) {
                        startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                    }
                }, "Not now", () -> notNow = true);
    }

    private void tellWhereTheGameGoes() {
        File shared = new File(Environment.getExternalStorageDirectory(), SHARED);
        show("Where the game goes", "Put the game in\n\n" + shared.getPath()
                + "\n\neither its files (default.xbe and the rest) or its disc image as one .iso file. A folder on an SD "
                + "card at the same place works too.\n\nYou can also pick the disc image here and the app copies it.\n\n"
                + "If something went wrong, COPY LOG puts this run's log on the clipboard.",
                "Import a disc image", this::startImport, "Copy log", this::copyLog, "Close", null);
    }

    private void startImport() {
        if (importer.pick()) return;
        tellWhereTheGameGoes();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == GameImporter.REQUEST) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) importImage(data.getData());
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    private void importImage(Uri uri) {
        File own = getExternalFilesDir(null);
        File target = own != null ? new File(own, "assets") : null;
        if (target == null) return;
        target.mkdirs();
        // the progress dialog comes up here, before Android gets round to onResume: without the flag the game search
        // put its "where the game goes" dialog over it and the import only showed once that was closed
        importing = true;
        if (dialog != null && dialog.isShowing()) dialog.dismiss();
        importer.copy(uri, target, ok -> {
            importing = false;
            if (ok) imported = true;
        });
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode == STORAGE_REQUEST) askForGame();
    }

    private void show(String title, String text, String positive, Runnable onPositive, String negative,
                      Runnable onNegative) {
        show(title, text, positive, onPositive, null, null, negative, onNegative);
    }

    private void show(String title, String text, String positive, Runnable onPositive, String neutral,
                      Runnable onNeutral, String negative, Runnable onNegative) {
        if (isFinishing()) return;
        if (dialog != null && dialog.isShowing()) dialog.dismiss();
        AlertDialog.Builder b = new AlertDialog.Builder(this)
                .setTitle(title)
                .setMessage(text)
                .setPositiveButton(positive, (d, w) -> {
                    if (onPositive != null) onPositive.run();
                })
                .setNegativeButton(negative, (d, w) -> {
                    if (onNegative != null) onNegative.run();
                });
        if (neutral != null) b.setNeutralButton(neutral, (d, w) -> {
            if (onNeutral != null) onNeutral.run();
        });
        dialog = b.show();
    }
}
