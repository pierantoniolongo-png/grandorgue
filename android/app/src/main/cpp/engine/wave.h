// Decoding of pipe samples: RIFF WAVE and WavPack, with loops and cue point.
// License GPL-2.0 or later.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace organ {

struct Loop {
  uint32_t start; // first frame of the loop
  uint32_t end;   // last frame of the loop (inclusive)
};

/** A decoded sample, stored as 16-bit interleaved PCM to save memory */
struct SampleData {
  std::vector<int16_t> pcm;
  int channels = 0;
  int rate = 0;
  uint32_t frames = 0;
  std::vector<Loop> loops;
  int64_t cue = -1; // release marker (frame), -1 if none
  int midiUnityNote = -1;
};

/** Decodes a WAV or WavPack file held in memory */
bool decodeAudio(
  const std::vector<uint8_t> &bytes, SampleData &out, std::string &err);

} // namespace organ
