// Organ list: installed packages, import of .orgue files, loading.
// License GPL-2.0 or later.
package org.organovirtuale.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Arrays;
import java.util.List;

public class MainActivity extends Activity {
  private static final int REQ_IMPORT = 1;
  private static final String DEMO_NAME = "GrandOrgue demo.orgue";

  private final Handler mUi = new Handler(Looper.getMainLooper());
  private LinearLayout mList;
  private TextView mStatus;
  private Button mOpenLoaded;

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);

    LinearLayout root = Ui.column(this);
    int pad = Ui.dp(this, 16);

    root.setPadding(pad, pad, pad, pad);
    root.setBackgroundColor(Ui.WOOD);
    root.addView(Ui.text(this, "Organo Virtuale", 26, true));
    root.addView(
        Ui.text(this, "Simulatore d'organo a canne compatibile con gli organi di GrandOrgue (.orgue)", 14, false));

    mStatus = Ui.text(this, "", 14, false);
    root.addView(mStatus, Ui.fill(this, 4));

    LinearLayout buttons = Ui.row(this);
    Button importButton = Ui.button(this, "Importa organo (.orgue)");

    importButton.setOnClickListener(v -> startImport());
    buttons.addView(importButton, Ui.wrap(this, 4));
    mOpenLoaded = Ui.button(this, "Torna all'organo");
    mOpenLoaded.setOnClickListener(v -> startActivity(new Intent(this, OrganActivity.class)));
    buttons.addView(mOpenLoaded, Ui.wrap(this, 4));
    root.addView(buttons);

    root.addView(Ui.text(this, "Organi installati", 18, true), Ui.fill(this, 8));

    ScrollView scroll = new ScrollView(this);

    mList = Ui.column(this);
    scroll.addView(mList);
    root.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

    root.addView(
        Ui.text(
            this,
            "Collega tastiere, pedaliera e interfaccia MIDI al telefono con un adattatore USB-C (OTG) e un hub"
                + " alimentato. Per l'audio usa il jack cuffie: il Bluetooth ha troppo ritardo per suonare.",
            12,
            false),
        Ui.fill(this, 4));
    setContentView(root);

    NativeEngine.startAudio();
    MidiHub hub = MidiHub.get(this);

    hub.addListener(MidiMapping.get(this));
    installDemo();
  }

  @Override
  protected void onResume() {
    super.onResume();
    refresh();
  }

  @Override
  protected void onDestroy() {
    super.onDestroy();
    if (isFinishing()) NativeEngine.stopAudio();
  }

  private File organDir() {
    File dir = new File(getFilesDir(), "organs");

    if (!dir.exists()) dir.mkdirs();
    return dir;
  }

  private void installDemo() {
    File demo = new File(organDir(), DEMO_NAME);

    if (demo.exists()) return;
    new Thread(
            () -> {
              try (InputStream in = getAssets().open("demo.orgue");
                  OutputStream out = new FileOutputStream(demo)) {
                byte[] buf = new byte[1 << 16];
                int n;

                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
              } catch (Exception e) {
                demo.delete();
              }
              mUi.post(this::refresh);
            })
        .start();
  }

  private void refresh() {
    MidiHub hub = MidiHub.get(this);
    List<String> devices = hub.deviceNames();
    String midi =
        !hub.isAvailable()
            ? "MIDI non disponibile su questo telefono"
            : devices.isEmpty() ? "Nessun dispositivo MIDI collegato" : "MIDI: " + String.join(", ", devices);
    String audio =
        NativeEngine.sampleRate() > 0
            ? "Audio " + NativeEngine.sampleRate() + " Hz, buffer " + NativeEngine.bufferFrames() + " campioni"
            : "Audio non avviato";

    mStatus.setText(midi + "\n" + audio);
    mOpenLoaded.setVisibility(OrganState.isLoaded() ? View.VISIBLE : View.GONE);
    if (OrganState.isLoaded())
      mOpenLoaded.setText("Torna a: " + OrganState.description.optString("name"));

    mList.removeAllViews();

    File[] files = organDir().listFiles();

    if (files == null || files.length == 0) {
      mList.addView(Ui.text(this, "Nessun organo: importa un file .orgue", 14, false));
      return;
    }
    Arrays.sort(files);
    for (File f : files) {
      if (f.getName().endsWith(".part")) continue;

      Button b = Ui.button(this, f.getName().replaceAll("\\.orgue$", "") + "   (" + Ui.formatSize(f.length()) + ")");

      b.setOnClickListener(v -> chooseAndLoad(f));
      b.setOnLongClickListener(
          v -> {
            confirmDelete(f);
            return true;
          });
      mList.addView(b, Ui.fill(this, 4));
    }
  }

  private void confirmDelete(File f) {
    new AlertDialog.Builder(this)
        .setTitle("Eliminare l'organo?")
        .setMessage(f.getName())
        .setPositiveButton(
            "Elimina",
            (d, w) -> {
              f.delete();
              refresh();
            })
        .setNegativeButton("Annulla", null)
        .show();
  }

  private void chooseAndLoad(File f) {
    try {
      JSONArray organs = new JSONArray(NativeEngine.text(NativeEngine.listOrgans(f.getAbsolutePath())));

      if (organs.length() > 1) {
        String[] names = new String[organs.length()];

        for (int i = 0; i < names.length; i++) names[i] = organs.getString(i);
        new AlertDialog.Builder(this)
            .setTitle("Quale organo?")
            .setItems(names, (d, which) -> load(f, names[which]))
            .show();
      } else load(f, "");
    } catch (Exception e) {
      load(f, "");
    }
  }

  private void load(File f, String odf) {
    LinearLayout box = Ui.column(this);
    ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
    int pad = Ui.dp(this, 20);

    bar.setMax(1000);
    box.setPadding(pad, pad, pad, pad);
    box.addView(bar, Ui.fill(this, 4));

    AlertDialog dialog =
        new AlertDialog.Builder(this).setTitle("Caricamento in corso…").setView(box).setCancelable(false).show();
    Runnable poll =
        new Runnable() {
          @Override
          public void run() {
            bar.setProgress((int) (NativeEngine.progress() * 1000));
            if (dialog.isShowing()) mUi.postDelayed(this, 150);
          }
        };

    mUi.post(poll);
    OrganState.description = null;
    new Thread(
            () -> {
              String err = NativeEngine.text(NativeEngine.load(f.getAbsolutePath(), odf));

              if (err.isEmpty()) {
                try {
                  OrganState.path = f.getAbsolutePath();
                  OrganState.refresh(MidiMapping.get(this));
                } catch (Exception e) {
                  err = e.getMessage();
                }
              }

              final String error = err;

              mUi.post(
                  () -> {
                    dialog.dismiss();
                    if (error.isEmpty()) startActivity(new Intent(this, OrganActivity.class));
                    else
                      new AlertDialog.Builder(this)
                          .setTitle("Impossibile caricare l'organo")
                          .setMessage(error)
                          .setPositiveButton("OK", null)
                          .show();
                    refresh();
                  });
            })
        .start();
  }

  private void startImport() {
    Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);

    i.addCategory(Intent.CATEGORY_OPENABLE);
    i.setType("*/*");
    startActivityForResult(i, REQ_IMPORT);
  }

  @Override
  protected void onActivityResult(int requestCode, int resultCode, Intent data) {
    super.onActivityResult(requestCode, resultCode, data);
    if (requestCode == REQ_IMPORT && resultCode == RESULT_OK && data != null && data.getData() != null)
      importFile(data.getData());
  }

  private String displayName(Uri uri) {
    String name = null;

    try (Cursor c = getContentResolver().query(uri, null, null, null, null)) {
      if (c != null && c.moveToFirst()) {
        int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);

        if (idx >= 0) name = c.getString(idx);
      }
    } catch (Exception ignored) {
      // fall back to a generic name
    }
    if (name == null || name.isEmpty()) name = "organo.orgue";
    return name.replace('/', '_');
  }

  private void importFile(Uri uri) {
    String name = displayName(uri);

    if (!name.toLowerCase().endsWith(".orgue")) {
      new AlertDialog.Builder(this)
          .setTitle("File non supportato")
          .setMessage(
              "Per ora si possono importare solo i pacchetti d'organo .orgue (il formato di GrandOrgue 3)."
                  + " Il file scelto è: " + name)
          .setPositiveButton("OK", null)
          .show();
      return;
    }

    LinearLayout box = Ui.column(this);
    TextView label = new TextView(this);
    int pad = Ui.dp(this, 20);

    box.setPadding(pad, pad, pad, pad);
    box.addView(label);

    AlertDialog dialog =
        new AlertDialog.Builder(this).setTitle("Importazione di " + name).setView(box).setCancelable(false).show();
    File dest = new File(organDir(), name);
    File part = new File(organDir(), name + ".part");

    new Thread(
            () -> {
              boolean isOk = true;
              long done = 0;

              try (InputStream in = getContentResolver().openInputStream(uri);
                  OutputStream out = new FileOutputStream(part)) {
                byte[] buf = new byte[1 << 20];
                int n;

                while ((n = in.read(buf)) > 0) {
                  out.write(buf, 0, n);
                  done += n;

                  final long d = done;

                  mUi.post(() -> label.setText("Copiati " + Ui.formatSize(d)));
                }
              } catch (Exception e) {
                isOk = false;
              }
              if (isOk) isOk = part.renameTo(dest);
              else part.delete();

              final boolean ok = isOk;

              mUi.post(
                  () -> {
                    dialog.dismiss();
                    if (!ok)
                      new AlertDialog.Builder(this)
                          .setTitle("Importazione non riuscita")
                          .setMessage("Controlla lo spazio libero sul telefono e riprova.")
                          .setPositiveButton("OK", null)
                          .show();
                    refresh();
                  });
            })
        .start();
  }
}
