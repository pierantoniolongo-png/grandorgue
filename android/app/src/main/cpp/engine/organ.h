// Organ model built from a GrandOrgue organ definition.
// License GPL-2.0 or later.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "source.h"
#include "wave.h"

namespace organ {

struct AttackInfo {
  int sample = -1;
  std::vector<Loop> loops; // empty: use the loops of the sample
  int64_t cue = -1;        // -1: use the cue point of the sample
  int maxKeyPressTime = -1;
  int isTremulant = -1; // -1 any, 0 without tremulant, 1 with tremulant
  int releaseCrossfadeMs = 0;
};

struct ReleaseInfo {
  int sample = -1;
  int maxKeyPressTime = -1;
  int isTremulant = -1;
  int releaseCrossfadeMs = 0;
};

struct Pipe {
  std::vector<AttackInfo> attacks;
  std::vector<ReleaseInfo> releases;
  float gain = 1.0f; // linear, including the 1/32768 sample scale
  double tuningCents = 0.0;
  int windchest = 0;
  bool isPercussive = false;
};

struct Rank {
  std::vector<int> pipes; // pipe ids, -1 for a dummy pipe
};

struct RankRef {
  int rank = 0;
  int firstPipe = 0; // 0-based pipe of the rank played by firstKey
  int pipeCount = 0;
  int firstKey = 0; // 0-based key of the stop
};

enum class DrawstopKind { SWITCH, STOP, COUPLER, TREMULANT };
enum class DrawstopFunc { INPUT, AND, OR, NOT, NAND, NOR, XOR };

/** Anything that can be on or off: switch, stop, coupler, tremulant */
struct Drawstop {
  std::string name;
  DrawstopKind kind = DrawstopKind::SWITCH;
  int target = -1; // index of the stop/coupler/tremulant
  DrawstopFunc func = DrawstopFunc::INPUT;
  std::vector<int> inputs; // drawstop ids of the controlling switches
  bool isDefaultOn = false;
  int gcState = 0; // state after general cancel; -1 unchanged
  int manual = -1; // manual it is shown in, -1 for the whole organ
};

struct Stop {
  std::string name;
  int manual = 0;
  int drawstop = -1;
  int firstKey = 0; // 0-based manual key of the first pipe
  int numKeys = 0;
  std::vector<RankRef> ranks;
  bool isEffects = false; // a single pipe sounding while the stop is on
};

struct Coupler {
  std::string name;
  int manual = 0;
  int dest = 0;
  int keyshift = 0;
  bool isUnisonOff = false;
  int drawstop = -1;
};

struct Tremulant {
  std::string name;
  int drawstop = -1;
  double periodMs = 200.0;
  double depth = 0.1; // amplitude modulation depth, 0..1
};

struct Enclosure {
  std::string name;
  float minLevel = 0.0f; // attenuation when closed, 0..1
};

struct Windchest {
  std::vector<int> enclosures;
  std::vector<int> tremulants;
};

struct Manual {
  std::string name;
  int odfNumber = 0;
  int firstMidiNote = 36;
  int firstLogicalKey = 1; // 1-based logical key of the first accessible key
  int numAccessible = 61;
  int numLogical = 61;
  std::vector<int> stops;
  std::vector<int> couplers;
  std::vector<int> drawstops; // drawstops shown with this manual
};

struct General {
  std::string name;
  std::vector<std::pair<int, bool>> settings; // drawstop id, state
};

struct Organ {
  std::string name;
  std::string builder;
  std::vector<SampleData> samples;
  std::vector<Pipe> pipes;
  std::vector<Rank> ranks;
  std::vector<Stop> stops;
  std::vector<Coupler> couplers;
  std::vector<Tremulant> tremulants;
  std::vector<Enclosure> enclosures;
  std::vector<Windchest> windchests;
  std::vector<Manual> manuals;
  std::vector<Drawstop> drawstops;
  std::vector<General> generals;
  size_t memoryBytes = 0;
  std::vector<std::string> warnings;
};

/**
 * Loads an organ. progress receives values from 0 to 1 while the samples
 * are decoded (it may be called from several threads).
 */
bool loadOrgan(
  FileSource &source,
  const std::string &odfPath,
  Organ &organ,
  const std::function<void(float)> &progress,
  std::string &err);

} // namespace organ
