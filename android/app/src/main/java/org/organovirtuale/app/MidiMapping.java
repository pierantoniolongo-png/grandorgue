// Assignments of MIDI devices/channels/controllers to the console, and the
// routing of incoming MIDI to the engine. The assignments do not depend on the
// organ: keyboards are stored by ODF manual number (0 = pedal), so every organ
// uses the same console setup.
// License GPL-2.0 or later.
package org.organovirtuale.app;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.HashMap;
import java.util.Map;

public final class MidiMapping implements MidiHub.Listener {
  public static final int TYPE_NOTE = 0x90;
  public static final int TYPE_CC = 0xB0;
  public static final int TYPE_PC = 0xC0;
  public static final int MAX_TOE_STUDS = 10;

  /** A learned MIDI source */
  public static final class Source {
    public final String device;
    public final int channel; // 1..16
    public final int type;    // TYPE_NOTE, TYPE_CC or TYPE_PC (keyboards: TYPE_NOTE)
    public final int number;  // controller, program or note; -1 for keyboards

    public Source(String device, int channel, int type, int number) {
      this.device = device;
      this.channel = channel;
      this.type = type;
      this.number = number;
    }

    String encode() {
      return device + "\n" + channel + "\n" + type + "\n" + number;
    }

    static Source decode(String s) {
      if (s == null) return null;
      String[] p = s.split("\n");
      if (p.length < 4) return null;
      try {
        return new Source(p[0], Integer.parseInt(p[1]), Integer.parseInt(p[2]), Integer.parseInt(p[3]));
      } catch (NumberFormatException e) {
        return null;
      }
    }

    public String describe() {
      String what =
          type == TYPE_NOTE
              ? (number < 0 ? "note" : "nota " + number)
              : type == TYPE_CC ? "controller CC" + number : "program change " + (number + 1);
      return device + ", canale " + channel + ", " + what;
    }

    boolean matches(String dev, int ch, int t, int n) {
      return device.equals(dev) && channel == ch && type == t && (number < 0 || number == n);
    }
  }

  /** Receives every MIDI message while the wizard is learning */
  public interface LearnListener {
    void onLearnMidi(String device, int status, int data1, int data2);
  }

  private static MidiMapping sInstance;

  private final SharedPreferences mPrefs;
  private final Map<Integer, Source> mKeyboards = new HashMap<>(); // ODF manual -> source
  private final Map<Integer, Source> mEnclosures = new HashMap<>();
  private final Map<Integer, Source> mToeStuds = new HashMap<>(); // general index -> source
  private Source mGeneralCancel;
  private volatile LearnListener mLearner;

  // the loaded organ: ODF manual number -> engine manual index
  private volatile Map<Integer, Integer> mManualIndex = new HashMap<>();
  private volatile int mFallbackManual = -1;

  public static synchronized MidiMapping get(Context context) {
    if (sInstance == null) sInstance = new MidiMapping(context.getApplicationContext());
    return sInstance;
  }

  private MidiMapping(Context context) {
    mPrefs = context.getSharedPreferences("midi", Context.MODE_PRIVATE);
    load();
  }

  private synchronized void load() {
    mKeyboards.clear();
    mEnclosures.clear();
    mToeStuds.clear();
    for (Map.Entry<String, ?> e : mPrefs.getAll().entrySet()) {
      Source s = Source.decode(String.valueOf(e.getValue()));
      String key = e.getKey();

      if (s == null) continue;
      try {
        if (key.startsWith("kbd.")) mKeyboards.put(Integer.parseInt(key.substring(4)), s);
        else if (key.startsWith("encl.")) mEnclosures.put(Integer.parseInt(key.substring(5)), s);
        else if (key.startsWith("toe.")) mToeStuds.put(Integer.parseInt(key.substring(4)), s);
        else if (key.equals("gc")) mGeneralCancel = s;
      } catch (NumberFormatException ignored) {
        // unknown key
      }
    }
  }

  public synchronized Source keyboard(int odfManual) {
    return mKeyboards.get(odfManual);
  }

  public synchronized Source enclosure(int index) {
    return mEnclosures.get(index);
  }

  public synchronized Source toeStud(int index) {
    return mToeStuds.get(index);
  }

  public synchronized Source generalCancel() {
    return mGeneralCancel;
  }

  public synchronized boolean hasKeyboards() {
    return !mKeyboards.isEmpty();
  }

  /** Replaces all assignments with the learned ones */
  public synchronized void save(
      Map<Integer, Source> keyboards,
      Map<Integer, Source> enclosures,
      Map<Integer, Source> toeStuds,
      Source generalCancel) {
    SharedPreferences.Editor ed = mPrefs.edit();

    for (Map.Entry<Integer, Source> e : keyboards.entrySet())
      ed.putString("kbd." + e.getKey(), e.getValue().encode());
    for (Map.Entry<Integer, Source> e : enclosures.entrySet())
      ed.putString("encl." + e.getKey(), e.getValue().encode());
    for (Map.Entry<Integer, Source> e : toeStuds.entrySet())
      ed.putString("toe." + e.getKey(), e.getValue().encode());
    if (generalCancel != null) ed.putString("gc", generalCancel.encode());
    ed.apply();
    mKeyboards.putAll(keyboards);
    mEnclosures.putAll(enclosures);
    mToeStuds.putAll(toeStuds);
    if (generalCancel != null) mGeneralCancel = generalCancel;
  }

  public synchronized void clear() {
    mPrefs.edit().clear().apply();
    load();
  }

  /** Tells the router which manuals the loaded organ has */
  public void setOrgan(Map<Integer, Integer> manualIndexByOdf, int fallbackManual) {
    mManualIndex = new HashMap<>(manualIndexByOdf);
    mFallbackManual = fallbackManual;
  }

  public void setLearner(LearnListener learner) {
    mLearner = learner;
  }

  @Override
  public void onMidi(String device, int status, int d1, int d2) {
    LearnListener learner = mLearner;

    if (learner != null) {
      learner.onLearnMidi(device, status, d1, d2);
      return;
    }

    int type = status & 0xF0;
    int ch = (status & 0x0F) + 1;

    synchronized (this) {
      if (type == 0x90 || type == 0x80) {
        boolean isOn = type == 0x90 && d2 > 0;
        boolean isRouted = false;

        for (Map.Entry<Integer, Source> e : mKeyboards.entrySet()) {
          Source s = e.getValue();

          if (s.device.equals(device) && s.channel == ch) {
            Integer manual = mManualIndex.get(e.getKey());

            if (manual != null) NativeEngine.noteOn(manual, d1, isOn ? Math.max(1, d2) : 0);
            isRouted = true;
          }
        }
        if (isOn) triggerButtons(device, ch, TYPE_NOTE, d1);
        // nothing configured yet: play the first manual
        if (!isRouted && mKeyboards.isEmpty() && mFallbackManual >= 0)
          NativeEngine.noteOn(mFallbackManual, d1, isOn ? Math.max(1, d2) : 0);
      } else if (type == 0xB0) {
        if (d1 == 120 || d1 == 123) NativeEngine.allNotesOff();
        for (Map.Entry<Integer, Source> e : mEnclosures.entrySet())
          if (e.getValue().matches(device, ch, TYPE_CC, d1)) NativeEngine.setEnclosure(e.getKey(), d2);
        if (d2 > 0) triggerButtons(device, ch, TYPE_CC, d1);
      } else if (type == 0xC0) {
        triggerButtons(device, ch, TYPE_PC, d1);
      }
    }
  }

  private void triggerButtons(String device, int ch, int type, int number) {
    for (Map.Entry<Integer, Source> e : mToeStuds.entrySet())
      if (e.getValue().matches(device, ch, type, number)) NativeEngine.general(e.getKey());
    if (mGeneralCancel != null && mGeneralCancel.matches(device, ch, type, number))
      NativeEngine.generalCancel();
  }
}
