// MIDI setup wizard: learns keyboards, expression pedals and toe studs.
// License GPL-2.0 or later.
package org.organovirtuale.app;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class WizardActivity extends Activity implements MidiMapping.LearnListener {
  private static final int KIND_KEYBOARD = 0;
  private static final int KIND_ENCLOSURE = 1;
  private static final int KIND_TOE = 2;
  private static final int KIND_GC = 3;

  private static final class Step {
    final int kind;
    final int index; // ODF manual, enclosure or general index
    final String title;
    final String instruction;
    MidiMapping.Source source;

    Step(int kind, int index, String title, String instruction) {
      this.kind = kind;
      this.index = index;
      this.title = title;
      this.instruction = instruction;
    }
  }

  private final Handler mUi = new Handler(Looper.getMainLooper());
  private final List<Step> mSteps = new ArrayList<>();
  private int mPos = 0;
  private boolean mIsAdvancing = false;

  private TextView mTitle;
  private TextView mInstruction;
  private TextView mStatus;
  private ProgressBar mProgress;
  private Button mBack;
  private Button mSkip;
  private Button mRepeat;
  private Button mSave;

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    if (!OrganState.isLoaded()) {
      finish();
      return;
    }
    buildSteps();
    buildUi();
    show();
  }

  @Override
  protected void onResume() {
    super.onResume();
    MidiMapping.get(this).setLearner(this);
  }

  @Override
  protected void onPause() {
    super.onPause();
    MidiMapping.get(this).setLearner(null);
  }

  private void buildSteps() {
    JSONObject d = OrganState.description;
    MidiMapping map = MidiMapping.get(this);
    JSONArray manuals = d.optJSONArray("manuals");
    JSONArray encl = d.optJSONArray("enclosures");
    JSONArray gens = d.optJSONArray("generals");

    for (int i = 0; manuals != null && i < manuals.length(); i++) {
      JSONObject m = manuals.optJSONObject(i);
      int odf = m.optInt("odf");
      String title = odf == 0 ? "Pedaliera" : "Manuale " + odf;
      Step s =
          new Step(
              KIND_KEYBOARD,
              odf,
              title + " (" + m.optString("name") + ")",
              odf == 0
                  ? "Premi un tasto qualsiasi della pedaliera."
                  : "Premi un tasto qualsiasi del manuale " + odf
                      + " (i manuali si contano dal basso: il manuale 1 è il più basso).");

      s.source = map.keyboard(odf);
      mSteps.add(s);
    }
    for (int i = 0; encl != null && i < encl.length(); i++) {
      JSONObject e = encl.optJSONObject(i);
      Step s =
          new Step(
              KIND_ENCLOSURE,
              e.optInt("id"),
              "Pedale d'espressione: " + e.optString("name"),
              "Muovi lentamente il pedale d'espressione da tutto chiuso a tutto aperto."
                  + " Sulla Behringer FCB1010 di solito è il pedale A.");

      s.source = map.enclosure(e.optInt("id"));
      mSteps.add(s);
    }
    for (int i = 0; gens != null && i < gens.length() && i < MidiMapping.MAX_TOE_STUDS; i++) {
      JSONObject g = gens.optJSONObject(i);
      Step s =
          new Step(
              KIND_TOE,
              g.optInt("id"),
              "Pedaletto " + (i + 1) + ": combinazione " + g.optString("name"),
              "Premi il pedaletto (pulsante a pedale) che deve richiamare la combinazione \""
                  + g.optString("name") + "\". Sulla FCB1010 premi il pulsante " + (i + 1) + ".");

      s.source = map.toeStud(g.optInt("id"));
      mSteps.add(s);
    }

    Step gc =
        new Step(
            KIND_GC,
            0,
            "Pedaletto: annulla registri",
            "Premi il pedaletto che deve spegnere tutti i registri (facoltativo: puoi saltarlo).");

    gc.source = map.generalCancel();
    mSteps.add(gc);
  }

  private void buildUi() {
    LinearLayout root = Ui.column(this);
    int pad = Ui.dp(this, 16);

    root.setBackgroundColor(Ui.WOOD);
    root.setPadding(pad, pad, pad, pad);
    root.addView(Ui.text(this, "Configura MIDI", 24, true));
    mProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
    mProgress.setMax(mSteps.size());
    root.addView(mProgress, Ui.fill(this, 6));

    ScrollView scroll = new ScrollView(this);
    LinearLayout body = Ui.column(this);

    mTitle = Ui.text(this, "", 22, true);
    mInstruction = Ui.text(this, "", 17, false);
    mStatus = Ui.text(this, "", 17, true);
    body.addView(mTitle, Ui.fill(this, 6));
    body.addView(mInstruction, Ui.fill(this, 6));
    body.addView(mStatus, Ui.fill(this, 12));
    scroll.addView(body);
    root.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

    LinearLayout row1 = Ui.row(this);

    mBack = Ui.button(this, "« Indietro");
    mRepeat = Ui.button(this, "Rileva di nuovo");
    mSkip = Ui.button(this, "Salta »");
    mBack.setOnClickListener(v -> go(mPos - 1));
    mRepeat.setOnClickListener(
        v -> {
          if (mPos < mSteps.size()) mSteps.get(mPos).source = null;
          show();
        });
    mSkip.setOnClickListener(v -> go(mPos + 1));
    row1.addView(mBack, Ui.wrap(this, 3));
    row1.addView(mRepeat, Ui.wrap(this, 3));
    row1.addView(mSkip, Ui.wrap(this, 3));
    root.addView(row1);

    LinearLayout row2 = Ui.row(this);
    Button cancel = Ui.button(this, "Annulla");

    mSave = Ui.button(this, "Salva");
    cancel.setOnClickListener(v -> finish());
    mSave.setOnClickListener(v -> save());
    row2.addView(cancel, Ui.wrap(this, 3));
    row2.addView(mSave, Ui.wrap(this, 3));
    root.addView(row2);
    setContentView(root);
  }

  private void go(int pos) {
    mPos = Math.max(0, Math.min(mSteps.size(), pos));
    mIsAdvancing = false;
    show();
  }

  private void show() {
    mProgress.setProgress(mPos);
    mBack.setEnabled(mPos > 0);
    if (mPos >= mSteps.size()) {
      StringBuilder sb = new StringBuilder();

      for (Step s : mSteps)
        sb.append(s.title).append(":\n   ").append(s.source == null ? "(non assegnato)" : s.source.describe()).append("\n");
      mTitle.setText("Riepilogo");
      mInstruction.setText(sb.toString());
      mStatus.setText("Tocca \"Salva\" per usare queste assegnazioni con tutti gli organi.");
      mSkip.setVisibility(View.GONE);
      mRepeat.setVisibility(View.GONE);
      return;
    }
    Step s = mSteps.get(mPos);

    mSkip.setVisibility(View.VISIBLE);
    mRepeat.setVisibility(View.VISIBLE);
    mTitle.setText("Passo " + (mPos + 1) + " di " + mSteps.size() + ": " + s.title);
    mInstruction.setText(s.instruction);
    mStatus.setTextColor(s.source == null ? Ui.STOP_ON : 0xFF9BE39B);
    mStatus.setText(s.source == null ? "In attesa di un segnale MIDI…" : "Assegnato: " + s.source.describe());
  }

  /** The step that already uses a source, or -1 */
  private int findOwner(String device, int ch, int type, int number, int except) {
    for (int i = 0; i < mSteps.size(); i++) {
      MidiMapping.Source src = mSteps.get(i).source;

      if (i == except || src == null || !src.device.equals(device) || src.channel != ch) continue;
      if (src.type == MidiMapping.TYPE_NOTE && src.number < 0 && type == MidiMapping.TYPE_NOTE) return i;
      if (src.type == type && src.number == number) return i;
    }
    return -1;
  }

  @Override
  public void onLearnMidi(String device, int status, int d1, int d2) {
    mUi.post(() -> handle(device, status, d1, d2));
  }

  private void handle(String device, int status, int d1, int d2) {
    if (mIsAdvancing || mPos >= mSteps.size()) return;

    Step s = mSteps.get(mPos);
    int type = status & 0xF0;
    int ch = (status & 0x0F) + 1;
    boolean isNoteOn = type == 0x90 && d2 > 0;
    boolean isUsableCc = type == 0xB0 && d1 != 0 && d1 != 32 && d1 < 120;
    MidiMapping.Source found = null;
    int owner = -1;

    if (s.kind == KIND_KEYBOARD && isNoteOn) {
      owner = findOwner(device, ch, MidiMapping.TYPE_NOTE, d1, mPos);
      if (owner < 0) found = new MidiMapping.Source(device, ch, MidiMapping.TYPE_NOTE, -1);
    } else if (s.kind == KIND_ENCLOSURE && isUsableCc) {
      owner = findOwner(device, ch, MidiMapping.TYPE_CC, d1, mPos);
      if (owner < 0) found = new MidiMapping.Source(device, ch, MidiMapping.TYPE_CC, d1);
    } else if (s.kind == KIND_TOE || s.kind == KIND_GC) {
      int t = type == 0xC0 ? MidiMapping.TYPE_PC : isNoteOn ? MidiMapping.TYPE_NOTE : (isUsableCc && d2 > 0) ? MidiMapping.TYPE_CC : -1;

      if (t >= 0) {
        owner = findOwner(device, ch, t, d1, mPos);
        if (owner < 0) found = new MidiMapping.Source(device, ch, t, d1);
      }
    }
    if (owner >= 0 && !(s.kind == KIND_ENCLOSURE && type == 0xB0)) {
      mStatus.setTextColor(0xFFFF8A80);
      mStatus.setText("Ignorato: questo segnale è già assegnato a \"" + mSteps.get(owner).title + "\".");
    }
    if (found != null) {
      s.source = found;
      mStatus.setTextColor(0xFF9BE39B);
      mStatus.setText("Rilevato: " + found.describe());
      mIsAdvancing = true;
      mUi.postDelayed(() -> go(mPos + 1), s.kind == KIND_ENCLOSURE ? 1500 : 700);
    }
  }

  private void save() {
    Map<Integer, MidiMapping.Source> kbd = new HashMap<>();
    Map<Integer, MidiMapping.Source> encl = new HashMap<>();
    Map<Integer, MidiMapping.Source> toe = new HashMap<>();
    MidiMapping.Source gc = null;

    for (Step s : mSteps) {
      if (s.source == null) continue;
      if (s.kind == KIND_KEYBOARD) kbd.put(s.index, s.source);
      else if (s.kind == KIND_ENCLOSURE) encl.put(s.index, s.source);
      else if (s.kind == KIND_TOE) toe.put(s.index, s.source);
      else gc = s.source;
    }
    MidiMapping.get(this).save(kbd, encl, toe, gc);
    finish();
  }
}
