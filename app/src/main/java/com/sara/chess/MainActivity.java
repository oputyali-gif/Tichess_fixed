package com.sara.chess;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Bundle;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * الواجهة كلها صفحة HTML داخل WebView (مجلد assets).
 * المحركات (Sara / Stockfish) ملفات تنفيذية أصلية تُشغَّل كعملية منفصلة ونتكلم معها بـ UCI.
 */
public class MainActivity extends Activity {

    private WebView web;
    private Process proc;
    private OutputStream procIn;
    private final Object lock = new Object();
    private File bookFile;

    @SuppressLint({"SetJavaScriptEnabled", "AddJavascriptInterface"})
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(0xFF1F1D1B);
        getWindow().setNavigationBarColor(0xFF1F1D1B);

        bookFile = new File(getFilesDir(), "openings.txt");
        copyBook();

        web = new WebView(this);
        web.setBackgroundColor(0xFF262421);
        setContentView(web);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(true);
        s.setSupportZoom(false);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setBuiltInZoomControls(false);

        web.addJavascriptInterface(new Bridge(), "Android");
        web.setWebViewClient(new WebViewClient());
        web.loadUrl("file:///android_asset/index.html");
    }

    @Override
    protected void onDestroy() {
        stopEngine();
        if (web != null) {
            web.destroy();
            web = null;
        }
        super.onDestroy();
    }

    private String readAsset(String name) {
        try (InputStream in = getAssets().open(name)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }

    private void copyBook() {
        try (FileOutputStream out = new FileOutputStream(bookFile)) {
            out.write(readAsset("book/openings.txt").getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    private File engineFile(String id) {
        String lib = "stockfish".equals(id) ? "libstockfish.so" : "libsara.so";
        return new File(getApplicationInfo().nativeLibraryDir, lib);
    }

    /** معلومات تشخيصية تظهر للمستخدم إذا لم يُعثر على أي محرك */
    private String diagText() {
        StringBuilder sb = new StringBuilder();
        File dir = new File(getApplicationInfo().nativeLibraryDir);
        sb.append("ABI: ").append(android.os.Build.SUPPORTED_ABIS.length > 0 ? android.os.Build.SUPPORTED_ABIS[0] : "?");
        sb.append(" | المجلد: ").append(dir.getAbsolutePath());
        String[] names = dir.list();
        sb.append(" | الملفات: ");
        if (names == null || names.length == 0) sb.append("(فارغ)");
        else for (String n : names) sb.append(n).append(' ');
        return sb.toString();
    }

    private void stopEngine() {
        synchronized (lock) {
            if (proc != null) {
                try {
                    if (procIn != null) {
                        procIn.write("quit\n".getBytes(StandardCharsets.UTF_8));
                        procIn.flush();
                    }
                } catch (IOException ignored) {
                }
                proc.destroy();
            }
            proc = null;
            procIn = null;
        }
    }

    private String startEngine(String id) {
        stopEngine();
        File exe = engineFile(id);
        if (!exe.exists()) return "الملف غير موجود: " + exe.getName();
        try {
            ProcessBuilder pb = new ProcessBuilder(exe.getAbsolutePath());
            pb.directory(getFilesDir());
            pb.redirectErrorStream(true);
            final Process p = pb.start();
            synchronized (lock) {
                proc = p;
                procIn = p.getOutputStream();
            }
            Thread t = new Thread(new Runnable() {
                @Override
                public void run() {
                    readLoop(p);
                }
            }, "engine-reader");
            t.setDaemon(true);
            t.start();
            return "ok";
        } catch (Exception e) {
            return String.valueOf(e.getMessage());
        }
    }

    private void readLoop(Process p) {
        try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                // نمرّر فقط ما تحتاجه الواجهة
                if (line.startsWith("info")) {
                    if (!line.contains(" score ")) continue;
                }
                js("window.onEngineLine&&window.onEngineLine(" + JSONObject.quote(line) + ")");
            }
        } catch (IOException ignored) {
        }
        boolean current;
        synchronized (lock) {
            current = (proc == p);
        }
        if (current) js("window.onEngineExit&&window.onEngineExit('انتهت عملية المحرك')");
    }

    private void js(final String code) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (web != null) web.evaluateJavascript(code, null);
            }
        });
    }

    /** الدوال التي تستدعيها الصفحة: Android.xxx() */
    public class Bridge {

        @JavascriptInterface
        public String engines() {
            JSONArray arr = new JSONArray();
            try {
                if (engineFile("stockfish").exists()) {
                    arr.put(new JSONObject().put("id", "stockfish").put("name", "Stockfish 19"));
                }
                if (engineFile("sara").exists()) {
                    arr.put(new JSONObject().put("id", "sara").put("name", "Sara"));
                }
            } catch (Exception ignored) {
            }
            return arr.toString();
        }

        @JavascriptInterface
        public String diag() {
            return diagText();
        }

        @JavascriptInterface
        public String startEngine(String id) {
            return MainActivity.this.startEngine(id);
        }

        @JavascriptInterface
        public void engineSend(String line) {
            synchronized (lock) {
                try {
                    if (procIn != null) {
                        procIn.write((line + "\n").getBytes(StandardCharsets.UTF_8));
                        procIn.flush();
                    }
                } catch (IOException ignored) {
                }
            }
        }

        @JavascriptInterface
        public String getBook() {
            return readAsset("book/openings.txt");
        }

        @JavascriptInterface
        public String bookPath() {
            return bookFile.getAbsolutePath();
        }

        @JavascriptInterface
        public int cores() {
            return Runtime.getRuntime().availableProcessors();
        }

        @JavascriptInterface
        public void copy(final String text) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
                    if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("PGN", text));
                    Toast.makeText(MainActivity.this, "تم نسخ PGN", Toast.LENGTH_SHORT).show();
                }
            });
        }

        @JavascriptInterface
        public void keepAwake(final boolean on) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    if (on) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                }
            });
        }
    }
}
