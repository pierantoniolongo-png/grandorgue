// Real-time organ engine: commands from any thread, rendering on the audio
// thread.
// License GPL-2.0 or later.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "organ.h"

namespace organ {

class Runtime;

class Engine {
public:
  Engine();
  ~Engine();

  /** Lists the organ definitions of a package or directory as JSON array */
  static std::string listOrgans(const std::string &path);

  /** Loads an organ (blocking, call it from a background thread) */
  bool load(const std::string &path, const std::string &odf, std::string &err);
  void unload();
  float loadProgress() const { return m_progress.load(); }
  bool isLoaded() const { return m_runtime.load() != nullptr; }

  /** Describes the loaded organ for the user interface (JSON) */
  std::string describe() const;

  // control, callable from any thread
  void noteOn(int manual, int note, int velocity);
  void noteOff(int manual, int note) { noteOn(manual, note, 0); }
  void setDrawstop(int drawstop, bool isOn);
  void setEnclosure(int enclosure, int value);
  void general(int general);
  void generalCancel();
  void allNotesOff();
  void setMasterGain(float gain);

  /** Current drawstop states (1 = on) */
  std::vector<uint8_t> drawstopStates() const;
  int activeVoices() const { return m_activeVoices.load(); }

  // audio thread
  void setOutputRate(int rate) { m_outputRate.store(rate); }
  /** Renders interleaved stereo float frames (always fills the buffer) */
  void render(float *out, int frames);

private:
  enum class CmdType : uint8_t {
    NOTE,
    DRAWSTOP,
    ENCLOSURE,
    GENERAL,
    GC,
    ALL_OFF,
    GAIN
  };
  struct Cmd {
    CmdType type;
    int32_t a;
    int32_t b;
    float f;
  };
  static constexpr size_t QUEUE_SIZE = 8192;

  std::vector<Cmd> m_queue;
  std::atomic<size_t> m_head{0}; // written by producers
  std::atomic<size_t> m_tail{0}; // written by the audio thread
  std::mutex m_pushMutex;

  std::atomic<Runtime *> m_runtime{nullptr};
  std::atomic<bool> m_isRendering{false};
  std::atomic<float> m_progress{0.0f};
  std::atomic<int> m_outputRate{48000};
  std::atomic<int> m_activeVoices{0};
  std::atomic<float> m_masterGain{0.3f};
  mutable std::mutex m_loadMutex;

  void push(const Cmd &cmd);
};

} // namespace organ
