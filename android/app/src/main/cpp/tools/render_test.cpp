// Desktop test: loads an organ, plays a chord and writes a WAV file.
// Usage: render_test <package-or-dir> <out.wav> [drawstop names...]
// License GPL-2.0 or later.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include "../engine/engine.h"

static void write_wav(const std::string &path, const std::vector<float> &s, int rate) {
  std::ofstream f(path, std::ios::binary);
  uint32_t dataLen = (uint32_t)(s.size() * 2);
  auto w32 = [&](uint32_t v) { f.write((const char *)&v, 4); };
  auto w16 = [&](uint16_t v) { f.write((const char *)&v, 2); };

  f.write("RIFF", 4);
  w32(36 + dataLen);
  f.write("WAVEfmt ", 8);
  w32(16);
  w16(1);
  w16(2);
  w32((uint32_t)rate);
  w32((uint32_t)rate * 4);
  w16(4);
  w16(16);
  f.write("data", 4);
  w32(dataLen);
  for (float x : s) {
    int v = (int)std::lrint(std::max(-1.0f, std::min(1.0f, x)) * 32767.0f);
    w16((uint16_t)(int16_t)v);
  }
}

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <organ> <out.wav> [drawstop names]\n", argv[0]);
    return 2;
  }

  organ::Engine engine;
  std::string err;
  const int rate = 48000;

  std::printf("organs: %s\n", organ::Engine::listOrgans(argv[1]).c_str());
  if (!engine.load(argv[1], "", err)) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }

  std::string desc = engine.describe();

  std::printf("%s\n", desc.c_str());
  engine.setOutputRate(rate);

  // engage the drawstops whose names contain the given words
  std::regex dsRe("\\{\"id\":(\\d+),\"name\":\"([^\"]*)\",\"kind\":\"(\\w+)\",\"input\":true\\}");

  for (auto it = std::sregex_iterator(desc.begin(), desc.end(), dsRe); it != std::sregex_iterator(); ++it)
    for (int a = 3; a < argc; a++)
      if ((*it)[2].str().find(argv[a]) != std::string::npos) {
        std::printf("engage %s (%s)\n", (*it)[2].str().c_str(), (*it)[1].str().c_str());
        engine.setDrawstop(std::stoi((*it)[1].str()), true);
      }

  std::vector<float> out;
  std::vector<float> buf(2 * 256);
  auto run = [&](double seconds) {
    int blocks = (int)(seconds * rate / 256);

    for (int b = 0; b < blocks; b++) {
      engine.render(buf.data(), 256);
      out.insert(out.end(), buf.begin(), buf.end());
    }
  };

  run(0.2);
  // C major chord on manual 1 (index 1 when the organ has a pedal)
  for (int n : {60, 64, 67})
    engine.noteOn(1, n, 100);
  engine.noteOn(0, 36, 100);
  run(2.5);
  std::printf("voices while holding: %d\n", engine.activeVoices());
  for (int n : {60, 64, 67})
    engine.noteOff(1, n);
  engine.noteOff(0, 36);
  run(2.0);
  std::printf("voices after release: %d\n", engine.activeVoices());

  double sum = 0.0;
  float peak = 0.0f;

  for (float x : out) {
    sum += (double)x * x;
    peak = std::max(peak, std::fabs(x));
  }
  std::printf("rms %.4f peak %.4f samples %zu\n", std::sqrt(sum / out.size()), peak, out.size());
  write_wav(argv[2], out, rate);
  return 0;
}
