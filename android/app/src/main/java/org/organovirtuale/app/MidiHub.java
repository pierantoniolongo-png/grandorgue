// Opens every MIDI input device (USB) and delivers complete messages.
// License GPL-2.0 or later.
package org.organovirtuale.app;

import android.content.Context;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;

import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;

public final class MidiHub {
  /** Receives complete MIDI channel messages */
  public interface Listener {
    void onMidi(String device, int status, int data1, int data2);
  }

  private static MidiHub sInstance;

  private final MidiManager mManager;
  private final Handler mHandler;
  private final Map<Integer, MidiDevice> mDevices = new HashMap<>();
  private final Map<Integer, List<String>> mNames = new HashMap<>();
  private final CopyOnWriteArrayList<Listener> mListeners = new CopyOnWriteArrayList<>();

  public static synchronized MidiHub get(Context context) {
    if (sInstance == null) sInstance = new MidiHub(context.getApplicationContext());
    return sInstance;
  }

  private MidiHub(Context context) {
    mManager = (MidiManager) context.getSystemService(Context.MIDI_SERVICE);

    HandlerThread thread = new HandlerThread("midi");

    thread.start();
    mHandler = new Handler(thread.getLooper());
    if (mManager != null) {
      mManager.registerDeviceCallback(
          new MidiManager.DeviceCallback() {
            @Override
            public void onDeviceAdded(MidiDeviceInfo info) {
              open(info);
            }

            @Override
            public void onDeviceRemoved(MidiDeviceInfo info) {
              close(info);
            }
          },
          mHandler);
      for (MidiDeviceInfo info : mManager.getDevices()) open(info);
    }
  }

  public boolean isAvailable() {
    return mManager != null;
  }

  public void addListener(Listener l) {
    mListeners.addIfAbsent(l);
  }

  public void removeListener(Listener l) {
    mListeners.remove(l);
  }

  /** Names of the connected input ports */
  public synchronized List<String> deviceNames() {
    List<String> result = new ArrayList<>();

    for (List<String> names : mNames.values()) result.addAll(names);
    return result;
  }

  private static String deviceName(MidiDeviceInfo info) {
    Bundle p = info.getProperties();
    String name = p.getString(MidiDeviceInfo.PROPERTY_NAME);

    if (name == null || name.isEmpty()) name = p.getString(MidiDeviceInfo.PROPERTY_PRODUCT);
    if (name == null || name.isEmpty()) name = "MIDI " + info.getId();
    return name;
  }

  private void open(final MidiDeviceInfo info) {
    if (info.getOutputPortCount() == 0) return;
    synchronized (this) {
      if (mDevices.containsKey(info.getId())) return;
      mDevices.put(info.getId(), null);
    }
    mManager.openDevice(
        info,
        device -> {
          if (device == null) {
            synchronized (MidiHub.this) {
              mDevices.remove(info.getId());
            }
            return;
          }

          String base = deviceName(info);
          List<String> names = new ArrayList<>();
          int nOutputs = info.getOutputPortCount();

          for (MidiDeviceInfo.PortInfo port : info.getPorts()) {
            if (port.getType() != MidiDeviceInfo.PortInfo.TYPE_OUTPUT) continue;

            MidiOutputPort out = device.openOutputPort(port.getPortNumber());

            if (out == null) continue;

            String name = nOutputs > 1 ? base + " #" + (port.getPortNumber() + 1) : base;

            out.connect(new Framer(name));
            names.add(name);
          }
          synchronized (MidiHub.this) {
            mDevices.put(info.getId(), device);
            mNames.put(info.getId(), names);
          }
        },
        mHandler);
  }

  private void close(MidiDeviceInfo info) {
    MidiDevice device;

    synchronized (this) {
      device = mDevices.remove(info.getId());
      mNames.remove(info.getId());
    }
    if (device != null) {
      try {
        device.close();
      } catch (IOException ignored) {
        // already gone
      }
    }
  }

  private void dispatch(String device, int status, int d1, int d2) {
    for (Listener l : mListeners) l.onMidi(device, status, d1, d2);
  }

  /** Splits the byte stream of one port into messages (running status aware) */
  private final class Framer extends MidiReceiver {
    private final String mName;
    private int mRunning = 0;
    private int mCount = 0;
    private int mNeeded = 0;
    private final int[] mData = new int[2];
    private boolean mIsSysex = false;

    Framer(String name) {
      mName = name;
    }

    private int lengthFor(int status) {
      int hi = status & 0xF0;

      if (hi == 0xC0 || hi == 0xD0) return 1;
      if (hi < 0xF0) return 2;
      if (status == 0xF1 || status == 0xF3) return 1;
      if (status == 0xF2) return 2;
      return 0;
    }

    @Override
    public void onSend(byte[] msg, int offset, int count, long timestamp) {
      for (int i = offset; i < offset + count; i++) {
        int b = msg[i] & 0xFF;

        if (b >= 0xF8) continue; // real time messages
        if (b == 0xF0) {
          mIsSysex = true;
          continue;
        }
        if (b == 0xF7) {
          mIsSysex = false;
          continue;
        }
        if (mIsSysex) {
          if (b < 0x80) continue;
          mIsSysex = false;
        }
        if (b >= 0x80) {
          mRunning = b;
          mCount = 0;
          mNeeded = lengthFor(b);
          if (b >= 0xF0) mRunning = 0;
          continue;
        }
        if (mRunning == 0) continue;
        mData[mCount++] = b;
        if (mCount >= mNeeded) {
          dispatch(mName, mRunning, mData[0], mNeeded > 1 ? mData[1] : 0);
          mCount = 0;
        }
      }
    }
  }
}
