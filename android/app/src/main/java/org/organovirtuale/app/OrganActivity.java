// The console: drawstops grouped by manual, generals, enclosures, volume.
// License GPL-2.0 or later.
package org.organovirtuale.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.GridLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class OrganActivity extends Activity {
  private final Handler mUi = new Handler(Looper.getMainLooper());
  private final Map<Integer, Button> mStopButtons = new HashMap<>();
  private byte[] mShownStates = new byte[0];
  private TextView mStatus;
  private TextView mHint;
  private boolean mIsPolling = false;

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    if (!OrganState.isLoaded()) {
      finish();
      return;
    }
    build();
  }

  @Override
  protected void onResume() {
    super.onResume();
    mIsPolling = true;
    mUi.post(mPoll);
    if (mHint != null)
      mHint.setVisibility(MidiMapping.get(this).hasKeyboards() ? View.GONE : View.VISIBLE);
  }

  @Override
  protected void onPause() {
    super.onPause();
    mIsPolling = false;
  }

  private final Runnable mPoll =
      new Runnable() {
        @Override
        public void run() {
          if (!mIsPolling) return;
          syncStates();
          List<String> devices = MidiHub.get(OrganActivity.this).deviceNames();

          mStatus.setText(
              (devices.isEmpty() ? "Nessun dispositivo MIDI" : "MIDI: " + String.join(", ", devices))
                  + "   ·   voci: " + NativeEngine.activeVoices()
                  + "   ·   " + NativeEngine.sampleRate() + " Hz / " + NativeEngine.bufferFrames());
          mUi.postDelayed(this, 200);
        }
      };

  private void syncStates() {
    byte[] states = NativeEngine.drawstopStates();

    if (states.length == 0) return;
    for (Map.Entry<Integer, Button> e : mStopButtons.entrySet()) {
      int id = e.getKey();

      if (id < states.length && (mShownStates.length != states.length || mShownStates[id] != states[id]))
        paint(e.getValue(), states[id] != 0);
    }
    mShownStates = states;
  }

  private void paint(Button b, boolean isOn) {
    b.setTag(isOn);
    b.setBackground(Ui.rounded(isOn ? Ui.STOP_ON : Ui.STOP_OFF, Ui.dp(this, 22), isOn ? Ui.RED : Color.BLACK));
    b.setTextColor(isOn ? Color.BLACK : Ui.IVORY);
  }

  private int columns() {
    DisplayMetrics dm = getResources().getDisplayMetrics();
    float widthDp = dm.widthPixels / dm.density;

    return Math.max(3, (int) (widthDp / 110));
  }

  private void build() {
    JSONObject d = OrganState.description;
    LinearLayout root = Ui.column(this);
    int pad = Ui.dp(this, 10);

    root.setBackgroundColor(Ui.WOOD);
    root.setPadding(pad, pad, pad, pad);

    // header
    TextView title = Ui.text(this, d.optString("name"), 22, true);

    root.addView(title);
    mStatus = Ui.text(this, "", 12, false);
    root.addView(mStatus);
    mHint =
        Ui.text(
            this,
            "Nessuna tastiera configurata: le note di qualsiasi tastiera suonano il primo manuale."
                + " Tocca \"Configura MIDI\" per assegnare tastiere, pedali e pedaletti.",
            13,
            true);
    mHint.setTextColor(Ui.STOP_ON);
    root.addView(mHint);

    int warnings = d.optInt("warningCount", 0);

    if (warnings > 0) {
      TextView w = Ui.text(this, warnings + " avvisi durante il caricamento (tocca per vederli)", 12, false);

      w.setOnClickListener(v -> showWarnings());
      root.addView(w);
    }

    LinearLayout actions = Ui.row(this);
    Button config = Ui.button(this, "Configura MIDI");
    Button gc = Ui.button(this, "Annulla registri");
    Button panic = Ui.button(this, "Silenzio");
    Button organs = Ui.button(this, "Organi");

    config.setOnClickListener(v -> startActivity(new Intent(this, WizardActivity.class)));
    gc.setOnClickListener(v -> NativeEngine.generalCancel());
    panic.setOnClickListener(v -> NativeEngine.allNotesOff());
    organs.setOnClickListener(v -> finish());
    actions.addView(config, Ui.wrap(this, 3));
    actions.addView(gc, Ui.wrap(this, 3));
    actions.addView(panic, Ui.wrap(this, 3));
    actions.addView(organs, Ui.wrap(this, 3));
    root.addView(actions);

    ScrollView scroll = new ScrollView(this);
    LinearLayout body = Ui.column(this);

    scroll.addView(body);
    root.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

    // volume
    body.addView(slider("Volume", 30, 100, v -> NativeEngine.setMasterGain(v / 100f)));

    // enclosures
    JSONArray encl = d.optJSONArray("enclosures");

    for (int i = 0; encl != null && i < encl.length(); i++) {
      JSONObject e = encl.optJSONObject(i);
      final int id = e.optInt("id");

      body.addView(slider("Espressione: " + e.optString("name"), 127, 127, v -> NativeEngine.setEnclosure(id, v)));
    }

    // generals
    JSONArray gens = d.optJSONArray("generals");

    if (gens != null && gens.length() > 0) {
      body.addView(Ui.text(this, "Combinazioni generali", 16, true), Ui.fill(this, 4));

      GridLayout grid = new GridLayout(this);

      grid.setColumnCount(Math.max(4, columns() + 2));
      for (int i = 0; i < gens.length(); i++) {
        JSONObject g = gens.optJSONObject(i);
        final int id = g.optInt("id");
        Button b = Ui.button(this, g.optString("name"));

        b.setOnClickListener(v -> NativeEngine.general(id));
        grid.addView(b);
      }
      body.addView(grid);
    }

    // drawstops: the highest manual first, the pedal last, like on a console
    JSONArray all = d.optJSONArray("drawstops");
    JSONArray manuals = d.optJSONArray("manuals");

    for (int m = manuals == null ? -1 : manuals.length() - 1; m >= 0; m--) {
      JSONObject man = manuals.optJSONObject(m);

      addGroup(body, man.optString("name"), man.optJSONArray("drawstops"), all);
    }
    addGroup(body, "Organo", d.optJSONArray("organDrawstops"), all);
    setContentView(root);
  }

  private interface IntConsumer {
    void accept(int v);
  }

  private View slider(String label, int initial, int max, IntConsumer onChange) {
    LinearLayout box = Ui.column(this);
    TextView t = Ui.text(this, label, 14, false);
    SeekBar bar = new SeekBar(this);

    bar.setMax(max);
    bar.setProgress(initial);
    bar.setOnSeekBarChangeListener(
        new SeekBar.OnSeekBarChangeListener() {
          @Override
          public void onProgressChanged(SeekBar s, int value, boolean fromUser) {
            if (fromUser) onChange.accept(value);
          }

          @Override
          public void onStartTrackingTouch(SeekBar s) {}

          @Override
          public void onStopTrackingTouch(SeekBar s) {}
        });
    onChange.accept(initial);
    box.addView(t);
    box.addView(bar, Ui.fill(this, 2));
    return box;
  }

  private void addGroup(LinearLayout body, String name, JSONArray ids, JSONArray all) {
    if (ids == null || ids.length() == 0 || all == null) return;

    TextView header = Ui.text(this, name, 18, true);

    header.setGravity(Gravity.CENTER_HORIZONTAL);
    body.addView(header, Ui.fill(this, 6));

    GridLayout grid = new GridLayout(this);
    int cols = columns();
    int size = (getResources().getDisplayMetrics().widthPixels - Ui.dp(this, 30)) / cols;

    grid.setColumnCount(cols);
    for (int i = 0; i < ids.length(); i++) {
      int id = ids.optInt(i);
      JSONObject ds = all.optJSONObject(id);

      if (ds == null) continue;

      Button b = new Button(this);
      GridLayout.LayoutParams lp = new GridLayout.LayoutParams();

      lp.width = size - Ui.dp(this, 6);
      lp.height = Ui.dp(this, 64);
      lp.setMargins(Ui.dp(this, 3), Ui.dp(this, 3), Ui.dp(this, 3), Ui.dp(this, 3));
      b.setLayoutParams(lp);
      b.setAllCaps(false);
      b.setText(ds.optString("name"));
      b.setTextSize(13);
      b.setMaxLines(3);
      paint(b, false);
      b.setOnClickListener(
          v -> {
            boolean isOn = !Boolean.TRUE.equals(v.getTag());

            paint((Button) v, isOn);
            NativeEngine.setDrawstop(id, isOn);
          });
      mStopButtons.put(id, b);
      grid.addView(b);
    }
    body.addView(grid);
  }

  private void showWarnings() {
    JSONArray w = OrganState.description.optJSONArray("warnings");
    StringBuilder sb = new StringBuilder();

    for (int i = 0; w != null && i < w.length(); i++) sb.append("• ").append(w.optString(i)).append('\n');
    new AlertDialog.Builder(this).setTitle("Avvisi").setMessage(sb.toString()).setPositiveButton("OK", null).show();
  }
}
