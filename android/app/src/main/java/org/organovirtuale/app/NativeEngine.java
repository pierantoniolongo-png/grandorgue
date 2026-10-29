// JNI access to the native organ engine.
// License GPL-2.0 or later.
package org.organovirtuale.app;

import java.nio.charset.StandardCharsets;

public final class NativeEngine {
  static {
    System.loadLibrary("organapp");
  }

  private NativeEngine() {}

  static native byte[] listOrgans(String path);

  /** Returns an empty array on success, otherwise the error message */
  static native byte[] load(String path, String odf);

  static native void unload();

  static native float progress();

  static native byte[] describe();

  static native void noteOn(int manual, int note, int velocity);

  static native void setDrawstop(int id, boolean isOn);

  static native void setEnclosure(int id, int value);

  static native void general(int id);

  static native void generalCancel();

  static native void allNotesOff();

  static native void setMasterGain(float gain);

  static native byte[] drawstopStates();

  static native int activeVoices();

  static native boolean startAudio();

  static native void stopAudio();

  static native int sampleRate();

  static native int bufferFrames();

  static String text(byte[] bytes) {
    return bytes == null ? "" : new String(bytes, StandardCharsets.UTF_8);
  }
}
