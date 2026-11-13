package com.sthrecompiled.shadow;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;

// Asks GitHub for the port's newest release, and offers it: "Update now" fetches the APK the release carries and hands
// it to Android's installer, "Later" leaves the phone alone. The installer keeps the app's data, so the layout, the
// settings and the imported disc image stay where they are.
//
// Nothing is checked at launch: the port only talks to GitHub when the player asks it to.
final class Updater {
    // the repository was renamed once already: the newest name first, the old one as a fallback
    static final String[] API = {"https://api.github.com/repos/Player124413/STH-Recompiled-android/releases/latest",
                                 "https://api.github.com/repos/Player124413/STH-Recompiled/releases/latest"};
    private static final String TAG = "ShadowUpdate";
    private static final int NOTES_CHARS = 700;

    private final Activity activity;
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile boolean cancelled;
    private File pending;  // downloaded and waiting for the "install unknown apps" permission

    Updater(Activity activity) {
        this.activity = activity;
    }

    private static final class Release {
        String tag = "", notes = "", page = "", apkName = "", apkUrl = "";
    }

    // current: the versionName this copy was built with
    void check(String current) {
        new Thread(() -> {
            Release release = null;
            String error = null;
            try {
                release = fetch();
            } catch (Exception e) {
                Log.i(TAG, "check failed", e);
                error = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            Release r = release;
            String err = error;
            main.post(() -> report(current, r, err));
        }, "ShadowUpdateCheck").start();
    }

    // null when the repository has no release yet
    private Release fetch() throws Exception {
        for (String api : API) {
            Release r = fetch(api);
            if (r != null) return r;  // null: this name has no release either, try the next
        }
        return null;
    }

    private Release fetch(String api) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(api).openConnection();
        c.setConnectTimeout(10000);
        c.setReadTimeout(15000);
        c.setRequestProperty("Accept", "application/vnd.github+json");
        c.setRequestProperty("User-Agent", "Shadow-Android");
        int code = c.getResponseCode();
        if (code == 404) return null;
        if (code != 200) throw new Exception("GitHub answered " + code);

        StringBuilder body = new StringBuilder();
        try (InputStream in = c.getInputStream()) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) body.append(new String(buf, 0, n, "UTF-8"));
        }
        JSONObject o = new JSONObject(body.toString());
        Release r = new Release();
        r.tag = o.optString("tag_name", "");
        r.notes = o.optString("body", "");
        r.page = o.optString("html_url", "");

        // the APK the release carries; a build with the game in it wins over a shell one when both are attached
        String shellName = null, shellUrl = null;
        JSONArray assets = o.optJSONArray("assets");
        if (assets != null) {
            for (int i = 0; i < assets.length(); i++) {
                JSONObject a = assets.getJSONObject(i);
                String name = a.optString("name", "");
                String url = a.optString("browser_download_url", "");
                if (!name.toLowerCase().endsWith(".apk") || url.isEmpty()) continue;
                if (name.toLowerCase().contains("shell")) {
                    if (shellUrl == null) {
                        shellName = name;
                        shellUrl = url;
                    }
                } else {
                    r.apkName = name;
                    r.apkUrl = url;
                    break;
                }
            }
        }
        if (r.apkUrl.isEmpty() && shellUrl != null) {
            r.apkName = shellName;
            r.apkUrl = shellUrl;
        }
        return r;
    }

    private void report(String current, Release r, String error) {
        if (activity.isFinishing()) return;
        if (error != null) {
            message("Check for updates", "GitHub could not be reached.\n\n" + error);
            return;
        }
        if (r == null || r.tag.isEmpty()) {
            message("Check for updates", "There is no release of the port on GitHub yet.\n\nThis copy is " + current + ".");
            return;
        }
        if (compare(r.tag, current) <= 0) {
            message("Check for updates", "This is the newest version (" + current + ").");
            return;
        }

        String notes = r.notes == null ? "" : r.notes.trim();
        if (notes.length() > NOTES_CHARS) notes = notes.substring(0, NOTES_CHARS).trim() + "…";
        String text = "Version " + r.tag + " is out; this phone has " + current + "."
                + (notes.isEmpty() ? "" : "\n\n" + notes);
        AlertDialog.Builder b = new AlertDialog.Builder(activity)
                .setTitle("Update available")
                .setMessage(text)
                .setNegativeButton("Later", null);
        if (!r.apkUrl.isEmpty()) {
            b.setPositiveButton("Update now", (d, w) -> download(r.apkUrl, r.apkName));
        } else if (!r.page.isEmpty()) {
            b.setPositiveButton("Open in browser", (d, w) -> open(r.page));
        } else {
            b.setPositiveButton("OK", null);
        }
        b.show();
    }

    private void download(String url, String name) {
        ProgressBar bar = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        TextView text = new TextView(activity);
        text.setText(name + "\nStarting…");
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(20 * activity.getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);
        box.addView(bar);
        box.addView(text);
        cancelled = false;
        AlertDialog progress = new AlertDialog.Builder(activity)
                .setTitle("Downloading the update")
                .setView(box)
                .setCancelable(false)
                .setNegativeButton("Cancel", (d, w) -> cancelled = true)
                .show();
        progress.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        File dir = new File(activity.getCacheDir(), "updates");
        dir.mkdirs();
        new Thread(() -> {
            String error = null;
            File part = new File(dir, "shadow-update.part");
            File apk = new File(dir, "shadow-update.apk");
            try {
                HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
                c.setConnectTimeout(10000);
                c.setReadTimeout(30000);
                c.setRequestProperty("User-Agent", "Shadow-Android");
                int code = c.getResponseCode();
                if (code / 100 != 2) throw new Exception("GitHub answered " + code);
                long total = c.getContentLengthLong();
                try (InputStream in = c.getInputStream(); FileOutputStream out = new FileOutputStream(part)) {
                    byte[] buf = new byte[1 << 16];
                    long got = 0, shown = 0;
                    int n;
                    while (!cancelled && (n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                        got += n;
                        if (got - shown >= (1 << 20)) {
                            shown = got;
                            long g = got;
                            main.post(() -> show(bar, text, name, g, total));
                        }
                    }
                    out.getFD().sync();
                }
                if (!cancelled && !part.renameTo(apk)) throw new Exception("the APK could not be put in place");
            } catch (Exception e) {
                Log.w(TAG, "download failed", e);
                error = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            boolean ok = error == null && !cancelled;
            String why = error;
            main.post(() -> {
                progress.dismiss();
                if (ok) install(apk);
                else if (why != null) message("Update failed", "The APK could not be downloaded.\n\n" + why);
            });
        }, "ShadowUpdateDownload").start();
    }

    private static void show(ProgressBar bar, TextView text, String name, long got, long total) {
        if (total > 0) {
            bar.setProgress((int) (got * 1000 / total));
            text.setText(String.format("%s\n%.1f of %.1f MB", name, got / 1e6, total / 1e6));
        } else {
            bar.setIndeterminate(true);
            text.setText(String.format("%s\n%.1f MB", name, got / 1e6));
        }
    }

    // Android's own installer, over a content:// the port serves: a file:// would be refused by the system
    private void install(File apk) {
        if (!activity.getPackageManager().canRequestPackageInstalls()) {
            pending = apk;
            message("Allow the update", "Android asks once: let this app install apps, and the update goes on by itself.");
            try {
                activity.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                        Uri.parse("package:" + activity.getPackageName())));
            } catch (Exception e) {
                Log.w(TAG, "no settings page for installs", e);
            }
            return;
        }
        Uri uri = Uri.parse("content://" + activity.getPackageName() + ".updates/" + apk.getName());
        Intent i = new Intent(Intent.ACTION_VIEW)
                .setDataAndType(uri, "application/vnd.android.package-archive")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        i.setClipData(ClipData.newRawUri("", uri));  // the grant rides along to the installer on every version
        try {
            activity.startActivity(i);
        } catch (Exception e) {
            message("Update failed", "No installer answered.\n\n" + e.getMessage());
        }
    }

    // back from the "install unknown apps" page: the update that was already downloaded goes ahead
    void afterPermissionScreen() {
        File apk = pending;
        if (apk == null || !apk.isFile() || !activity.getPackageManager().canRequestPackageInstalls()) return;
        pending = null;
        main.post(() -> install(apk));
    }

    private void open(String url) {
        try {
            activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (Exception e) {
            message("No browser", "Nothing on this phone can open\n\n" + url);
        }
    }

    private void message(String title, String text) {
        if (activity.isFinishing()) return;
        new AlertDialog.Builder(activity).setTitle(title).setMessage(text).setPositiveButton("OK", null).show();
    }

    // a tag against a versionName: 1.2.0 and v1.2 are equal, 1.10 is newer than 1.9, a suffix is ignored
    static int compare(String a, String b) {
        List<Integer> x = numbers(a), y = numbers(b);
        for (int i = 0; i < Math.max(x.size(), y.size()); i++) {
            int p = i < x.size() ? x.get(i) : 0;
            int q = i < y.size() ? y.get(i) : 0;
            if (p != q) return p < q ? -1 : 1;
        }
        return 0;
    }

    private static List<Integer> numbers(String v) {
        List<Integer> out = new ArrayList<>();
        for (String p : v.replaceAll("[^0-9.]", ".").split("\\.")) {
            if (p.isEmpty()) continue;
            try {
                out.add(Integer.parseInt(p));
            } catch (NumberFormatException e) {
                out.add(0);
            }
        }
        return out;
    }
}
