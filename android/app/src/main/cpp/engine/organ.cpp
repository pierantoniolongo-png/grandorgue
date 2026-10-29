// Organ model built from a GrandOrgue organ definition.
// License GPL-2.0 or later.
#include "organ.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "odf.h"

namespace organ {

namespace {

std::string lower(const std::string &s) {
  std::string r(s);

  for (char &c : r)
    c = (char)std::tolower((unsigned char)c);
  return r;
}

/** Hierarchical pipe parameters: organ, windchest, rank, pipe */
struct Level {
  double amp = 1.0;     // product of AmplitudeLevel / 100
  double gainDb = 0.0;  // sum of Gain
  double tuning = 0.0;  // sum of PitchTuning (cents)

  Level combine(const Level &o) const {
    return Level{amp * o.amp, gainDb + o.gainDb, tuning + o.tuning};
  }
};

struct PendingRef {
  int rank;
  int index;
  int manual; // ODF manual number
  int stop;   // 1-based stop of the manual
  int pipe;   // 1-based pipe of the first rank of the stop
};

class Loader {
public:
  Loader(const OdfFile &odf, FileSource &src, Organ &o, const std::string &dir)
    : r_odf(odf), r_src(src), r_organ(o), m_dir(dir) {}

  bool run(const std::function<void(float)> &progress, std::string &err);

private:
  const OdfFile &r_odf;
  FileSource &r_src;
  Organ &r_organ;
  std::string m_dir;

  Level m_organLevel;
  std::vector<Level> m_windchestLevels;
  std::unordered_map<std::string, int> m_sampleIndex;
  std::vector<std::string> m_sampleFiles;
  std::map<int, int> m_switchDrawstop;   // switch number -> drawstop id
  std::map<int, int> m_stopByNumber;     // global stop number -> stop id
  std::map<int, int> m_couplerByNumber;  // global coupler number -> coupler id
  std::map<int, int> m_tremulantByNumber; // tremulant number -> tremulant id
  std::map<int, int> m_manualByOdf;      // ODF manual number -> manual index
  std::vector<std::vector<int>> m_rawInputs; // per drawstop: switch numbers
  std::vector<PendingRef> m_refs;

  Level readLevel(const std::string &section, const std::string &prefix) const;
  int sampleFor(const std::string &path);
  int addDrawstop(const std::string &section, DrawstopKind kind, int target);
  void loadAttack(
    const std::string &section, const std::string &prefix, const std::string &file, Pipe &pipe);
  int loadPipe(
    const std::string &section,
    const std::string &prefix,
    const Level &rankLevel,
    int rankWindchest,
    bool isRankPercussive,
    int rankIndex,
    int pipeIndex);
  int loadRank(const std::string &section, int nPipes);
  int loadStop(int number, int manual);
  int loadCoupler(int number, int manual);
  bool resolveRefs(std::string &err);
  void loadGenerals();
  bool decodeSamples(const std::function<void(float)> &progress, std::string &err);
};

Level Loader::readLevel(const std::string &section, const std::string &prefix) const {
  Level l;

  l.amp = r_odf.getFloat(section, prefix + "AmplitudeLevel", 100.0) / 100.0;
  l.gainDb = r_odf.getFloat(section, prefix + "Gain", 0.0);
  l.tuning = r_odf.getFloat(section, prefix + "PitchTuning", 0.0);
  return l;
}

int Loader::sampleFor(const std::string &path) {
  std::string full = normalizePath(m_dir + normalizePath(path));
  std::string key = lower(full);
  auto it = m_sampleIndex.find(key);
  int index;

  if (it != m_sampleIndex.end())
    index = it->second;
  else {
    index = (int)m_sampleFiles.size();
    m_sampleIndex[key] = index;
    m_sampleFiles.push_back(full);
  }
  return index;
}

int Loader::addDrawstop(const std::string &section, DrawstopKind kind, int target) {
  Drawstop d;
  std::string func = lower(r_odf.get(section, "Function", "input"));
  std::vector<int> inputs;

  d.name = toUtf8(r_odf.get(section, "Name", section));
  d.kind = kind;
  d.target = target;
  d.isDefaultOn = r_odf.getBool(section, "DefaultToEngaged", false);
  d.gcState = (int)r_odf.getInt(section, "GCState", 0);
  d.func = func == "and" ? DrawstopFunc::AND
    : func == "or"       ? DrawstopFunc::OR
    : func == "not"      ? DrawstopFunc::NOT
    : func == "nand"     ? DrawstopFunc::NAND
    : func == "nor"      ? DrawstopFunc::NOR
    : func == "xor"      ? DrawstopFunc::XOR
                         : DrawstopFunc::INPUT;
  if (d.func != DrawstopFunc::INPUT) {
    long n = r_odf.getInt(section, "SwitchCount", 0);

    for (long i = 1; i <= n; i++)
      inputs.push_back((int)r_odf.getInt(section, num3("Switch", (int)i), 0));
  }
  r_organ.drawstops.push_back(d);
  m_rawInputs.push_back(inputs);
  return (int)r_organ.drawstops.size() - 1;
}

void Loader::loadAttack(
  const std::string &section, const std::string &prefix, const std::string &file, Pipe &pipe) {
  AttackInfo a;
  long nLoops = r_odf.getInt(section, prefix + "LoopCount", 0);

  a.sample = sampleFor(file);
  a.cue = r_odf.getInt(section, prefix + "CuePoint", -1);
  if (!r_odf.getBool(section, prefix + "LoadRelease", !pipe.isPercussive))
    a.cue = -2; // the release part of the sample must not be used
  a.maxKeyPressTime = (int)r_odf.getInt(section, prefix + "MaxKeyPressTime", -1);
  a.isTremulant = (int)r_odf.getInt(section, prefix + "IsTremulant", -1);
  a.releaseCrossfadeMs = (int)r_odf.getInt(section, prefix + "ReleaseCrossfadeLength", 0);
  for (long i = 1; i <= nLoops; i++) {
    std::string lp = num3(prefix + "Loop", (int)i);
    long start = r_odf.getInt(section, lp + "Start", 0);
    long end = r_odf.getInt(section, lp + "End", 0);

    if (end > start)
      a.loops.push_back(Loop{(uint32_t)start, (uint32_t)end});
  }
  pipe.attacks.push_back(a);
}

int Loader::loadPipe(
  const std::string &section,
  const std::string &prefix,
  const Level &rankLevel,
  int rankWindchest,
  bool isRankPercussive,
  int rankIndex,
  int pipeIndex) {
  std::string value = r_odf.get(section, prefix, "");
  int pipeId = -1;

  if (value.rfind("REF:", 0) == 0 || value.rfind("ref:", 0) == 0) {
    PendingRef ref{rankIndex, pipeIndex, 0, 0, 0};

    if (std::sscanf(value.c_str() + 4, "%d:%d:%d", &ref.manual, &ref.stop, &ref.pipe) == 3)
      m_refs.push_back(ref);
  } else if (!value.empty() && lower(value) != "dummy") {
    Pipe p;
    int windchest = (int)r_odf.getInt(section, prefix + "WindchestGroup", rankWindchest);
    Level wcLevel = windchest >= 1 && windchest <= (int)m_windchestLevels.size()
      ? m_windchestLevels[(size_t)windchest - 1]
      : Level();
    Level level = m_organLevel.combine(wcLevel).combine(rankLevel).combine(
      readLevel(section, prefix));
    long nAttacks = r_odf.getInt(section, prefix + "AttackCount", 0);
    long nReleases = r_odf.getInt(section, prefix + "ReleaseCount", 0);

    p.windchest = std::max(0, windchest - 1);
    p.isPercussive = r_odf.getBool(section, prefix + "Percussive", isRankPercussive);
    p.gain = (float)(level.amp * std::pow(10.0, level.gainDb / 20.0) / 32768.0);
    p.tuningCents = level.tuning;
    loadAttack(section, prefix, value, p);
    for (long i = 1; i <= nAttacks; i++) {
      std::string ap = num3(prefix + "Attack", (int)i);
      std::string file = r_odf.get(section, ap, "");

      if (!file.empty())
        loadAttack(section, ap, file, p);
    }
    for (long i = 1; i <= nReleases; i++) {
      std::string rp = num3(prefix + "Release", (int)i);
      std::string file = r_odf.get(section, rp, "");

      if (!file.empty()) {
        ReleaseInfo rel;

        rel.sample = sampleFor(file);
        rel.maxKeyPressTime = (int)r_odf.getInt(section, rp + "MaxKeyPressTime", -1);
        rel.isTremulant = (int)r_odf.getInt(section, rp + "IsTremulant", -1);
        rel.releaseCrossfadeMs = (int)r_odf.getInt(section, rp + "ReleaseCrossfadeLength", 0);
        p.releases.push_back(rel);
      }
    }
    r_organ.pipes.push_back(p);
    pipeId = (int)r_organ.pipes.size() - 1;
  }
  return pipeId;
}

int Loader::loadRank(const std::string &section, int nPipes) {
  Level level = readLevel(section, "");
  int windchest = (int)r_odf.getInt(section, "WindchestGroup", 1);
  bool isPercussive = r_odf.getBool(section, "Percussive", false);
  int rankIndex = (int)r_organ.ranks.size();

  r_organ.ranks.push_back(Rank());
  r_organ.ranks.back().pipes.resize((size_t)std::max(0, nPipes), -1);
  for (int i = 0; i < nPipes; i++) {
    int id = loadPipe(
      section, num3("Pipe", i + 1), level, windchest, isPercussive, rankIndex, i);

    r_organ.ranks[(size_t)rankIndex].pipes[(size_t)i] = id;
  }
  return rankIndex;
}

int Loader::loadStop(int number, int manual) {
  auto it = m_stopByNumber.find(number);
  int stopId;

  if (it != m_stopByNumber.end())
    stopId = it->second;
  else {
    std::string section = num3("Stop", number);
    Stop s;
    long nRanks = r_odf.getInt(section, "NumberOfRanks", 0);

    s.name = toUtf8(r_odf.get(section, "Name", section));
    s.manual = manual;
    s.firstKey = (int)r_odf.getInt(section, "FirstAccessiblePipeLogicalKeyNumber", 1) - 1;
    s.numKeys = (int)r_odf.getInt(section, "NumberOfAccessiblePipes", 1);
    if (nRanks > 0) {
      for (long i = 1; i <= nRanks; i++) {
        std::string rp = num3("Rank", (int)i);
        int rankNo = (int)r_odf.getInt(section, rp, 0);

        if (rankNo >= 1 && rankNo <= (int)r_organ.ranks.size()) {
          RankRef ref;
          int nRankPipes = (int)r_organ.ranks[(size_t)rankNo - 1].pipes.size();

          ref.rank = rankNo - 1;
          ref.firstPipe = (int)r_odf.getInt(section, rp + "FirstPipeNumber", 1) - 1;
          ref.pipeCount = (int)r_odf.getInt(
            section, rp + "PipeCount", nRankPipes - ref.firstPipe);
          ref.firstKey = (int)r_odf.getInt(section, rp + "FirstAccessibleKeyNumber", 1) - 1;
          s.ranks.push_back(ref);
        }
      }
    } else {
      RankRef ref;

      ref.rank = loadRank(section, (int)r_odf.getInt(section, "NumberOfLogicalPipes", s.numKeys));
      ref.firstPipe = (int)r_odf.getInt(section, "FirstAccessiblePipeLogicalPipeNumber", 1) - 1;
      ref.pipeCount = s.numKeys;
      ref.firstKey = 0;
      s.ranks.push_back(ref);
    }
    s.isEffects = s.ranks.size() == 1 && r_organ.ranks[(size_t)s.ranks[0].rank].pipes.size() == 1;
    stopId = (int)r_organ.stops.size();
    s.drawstop = addDrawstop(section, DrawstopKind::STOP, stopId);
    r_organ.stops.push_back(s);
    m_stopByNumber[number] = stopId;
  }
  return stopId;
}

int Loader::loadCoupler(int number, int manual) {
  auto it = m_couplerByNumber.find(number);
  int couplerId;

  if (it != m_couplerByNumber.end())
    couplerId = it->second;
  else {
    std::string section = num3("Coupler", number);
    Coupler c;
    int destOdf = (int)r_odf.getInt(section, "DestinationManual", -1);
    auto dit = m_manualByOdf.find(destOdf);

    c.name = toUtf8(r_odf.get(section, "Name", section));
    c.manual = manual;
    c.isUnisonOff = r_odf.getBool(section, "UnisonOff", false);
    c.dest = dit != m_manualByOdf.end() ? dit->second : -1;
    c.keyshift = (int)r_odf.getInt(section, "DestinationKeyshift", 0);
    couplerId = (int)r_organ.couplers.size();
    c.drawstop = addDrawstop(section, DrawstopKind::COUPLER, couplerId);
    r_organ.couplers.push_back(c);
    m_couplerByNumber[number] = couplerId;
  }
  return couplerId;
}

bool Loader::resolveRefs(std::string &err) {
  // References may point to pipes that are references themselves
  for (int pass = 0; pass < 8 && !m_refs.empty(); pass++) {
    std::vector<PendingRef> remaining;

    for (const PendingRef &r : m_refs) {
      auto mit = m_manualByOdf.find(r.manual);
      int pipeId = -1;
      bool isResolved = false;

      if (mit != m_manualByOdf.end()) {
        const Manual &m = r_organ.manuals[(size_t)mit->second];

        if (r.stop >= 1 && r.stop <= (int)m.stops.size()) {
          const Stop &s = r_organ.stops[(size_t)m.stops[(size_t)r.stop - 1]];
          const Rank &rank = r_organ.ranks[(size_t)s.ranks[0].rank];

          if (r.pipe >= 1 && r.pipe <= (int)rank.pipes.size()) {
            pipeId = rank.pipes[(size_t)r.pipe - 1];
            isResolved = pipeId >= 0;
          }
        }
      }
      if (isResolved)
        r_organ.ranks[(size_t)r.rank].pipes[(size_t)r.index] = pipeId;
      else
        remaining.push_back(r);
    }
    if (remaining.size() == m_refs.size())
      break;
    m_refs.swap(remaining);
  }
  if (!m_refs.empty())
    r_organ.warnings.push_back(
      std::to_string(m_refs.size()) + " pipe references could not be resolved");
  (void)err;
  return true;
}

void Loader::loadGenerals() {
  long n = r_odf.getInt("Organ", "NumberOfGenerals", 0);

  for (long g = 1; g <= n; g++) {
    std::string section = num3("General", (int)g);
    General gen;

    gen.name = toUtf8(r_odf.get(section, "Name", std::to_string(g)));

    long nStops = r_odf.getInt(section, "NumberOfStops", 0);

    for (long i = 1; i <= nStops; i++) {
      int manOdf = (int)r_odf.getInt(section, num3("StopManual", (int)i), -1);
      int num = (int)r_odf.getInt(section, num3("StopNumber", (int)i), 0);
      auto mit = m_manualByOdf.find(manOdf);

      if (mit != m_manualByOdf.end() && num != 0) {
        const Manual &m = r_organ.manuals[(size_t)mit->second];
        int idx = std::abs(num);

        if (idx <= (int)m.stops.size())
          gen.settings.emplace_back(
            r_organ.stops[(size_t)m.stops[(size_t)idx - 1]].drawstop, num > 0);
      }
    }

    long nSwitches = r_odf.getInt(section, "NumberOfSwitches", 0);

    for (long i = 1; i <= nSwitches; i++) {
      int num = (int)r_odf.getInt(section, num3("SwitchNumber", (int)i), 0);
      auto sit = m_switchDrawstop.find(std::abs(num));

      if (num != 0 && sit != m_switchDrawstop.end())
        gen.settings.emplace_back(sit->second, num > 0);
    }

    long nCouplers = r_odf.getInt(section, "NumberOfCouplers", 0);

    for (long i = 1; i <= nCouplers; i++) {
      int manOdf = (int)r_odf.getInt(section, num3("CouplerManual", (int)i), -1);
      int num = (int)r_odf.getInt(section, num3("CouplerNumber", (int)i), 0);
      auto mit = m_manualByOdf.find(manOdf);

      if (mit != m_manualByOdf.end() && num != 0) {
        const Manual &m = r_organ.manuals[(size_t)mit->second];
        int idx = std::abs(num);

        if (idx <= (int)m.couplers.size())
          gen.settings.emplace_back(
            r_organ.couplers[(size_t)m.couplers[(size_t)idx - 1]].drawstop, num > 0);
      }
    }

    long nTrems = r_odf.getInt(section, "NumberOfTremulants", 0);

    for (long i = 1; i <= nTrems; i++) {
      int num = (int)r_odf.getInt(section, num3("TremulantNumber", (int)i), 0);
      auto tit = m_tremulantByNumber.find(std::abs(num));

      if (num != 0 && tit != m_tremulantByNumber.end())
        gen.settings.emplace_back(
          r_organ.tremulants[(size_t)tit->second].drawstop, num > 0);
    }
    r_organ.generals.push_back(gen);
  }
}

bool Loader::decodeSamples(const std::function<void(float)> &progress, std::string &err) {
  const size_t nFiles = m_sampleFiles.size();
  std::atomic<size_t> next{0};
  std::atomic<size_t> done{0};
  std::mutex warnMutex;
  unsigned nThreads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
  std::vector<std::thread> threads;

  r_organ.samples.resize(nFiles);

  auto worker = [&]() {
    std::vector<uint8_t> bytes;

    for (size_t i = next++; i < nFiles; i = next++) {
      std::string e;

      if (!r_src.read(m_sampleFiles[i], bytes))
        e = "missing file " + m_sampleFiles[i];
      else if (!decodeAudio(bytes, r_organ.samples[i], e))
        e = m_sampleFiles[i] + ": " + e;
      if (!e.empty()) {
        std::lock_guard<std::mutex> lock(warnMutex);

        r_organ.warnings.push_back(e);
      }

      size_t d = ++done;

      if (progress && (d % 16 == 0 || d == nFiles))
        progress((float)d / (float)std::max<size_t>(1, nFiles));
    }
  };

  for (unsigned t = 0; t < nThreads; t++)
    threads.emplace_back(worker);
  for (auto &t : threads)
    t.join();

  for (const SampleData &s : r_organ.samples)
    r_organ.memoryBytes += s.pcm.size() * sizeof(int16_t);
  (void)err;
  return true;
}

bool Loader::run(const std::function<void(float)> &progress, std::string &err) {
  bool isOk = r_odf.hasSection("Organ");

  if (!isOk)
    err = "Not an organ definition file: [Organ] section missing";
  if (isOk) {
    r_organ.name = toUtf8(r_odf.get("Organ", "ChurchName", "Organ"));
    r_organ.builder = toUtf8(r_odf.get("Organ", "OrganBuilder", ""));
    m_organLevel = readLevel("Organ", "");

    // enclosures
    long nEnclosures = r_odf.getInt("Organ", "NumberOfEnclosures", 0);

    for (long i = 1; i <= nEnclosures; i++) {
      std::string section = num3("Enclosure", (int)i);
      Enclosure e;

      e.name = toUtf8(r_odf.get(section, "Name", section));
      e.minLevel = (float)r_odf.getInt(section, "AmpMinimumLevel", 0) / 100.0f;
      r_organ.enclosures.push_back(e);
    }

    // switches (their logic inputs are resolved at the end)
    long nSwitches = r_odf.getInt("Organ", "NumberOfSwitches", 0);

    for (long i = 1; i <= nSwitches; i++)
      m_switchDrawstop[(int)i] = addDrawstop(num3("Switch", (int)i), DrawstopKind::SWITCH, (int)i);

    // tremulants
    long nTrems = r_odf.getInt("Organ", "NumberOfTremulants", 0);

    for (long i = 1; i <= nTrems; i++) {
      std::string section = num3("Tremulant", (int)i);
      Tremulant t;
      int id = (int)r_organ.tremulants.size();

      t.name = toUtf8(r_odf.get(section, "Name", section));
      t.periodMs = std::max(30.0, r_odf.getFloat(section, "Period", 200.0));
      t.depth = lower(r_odf.get(section, "TremulantType", "synth")) == "wave"
        ? 0.0
        : r_odf.getFloat(section, "AmpModDepth", 18.0) / 100.0;
      t.drawstop = addDrawstop(section, DrawstopKind::TREMULANT, id);
      r_organ.tremulants.push_back(t);
      m_tremulantByNumber[(int)i] = id;
    }

    // windchests
    long nWindchests = r_odf.getInt("Organ", "NumberOfWindchestGroups", 0);

    for (long i = 1; i <= nWindchests; i++) {
      std::string section = num3("WindchestGroup", (int)i);
      Windchest w;
      long ne = r_odf.getInt(section, "NumberOfEnclosures", 0);
      long nt = r_odf.getInt(section, "NumberOfTremulants", 0);

      for (long k = 1; k <= ne; k++) {
        int e = (int)r_odf.getInt(section, num3("Enclosure", (int)k), 0);

        if (e >= 1 && e <= (int)r_organ.enclosures.size())
          w.enclosures.push_back(e - 1);
      }
      for (long k = 1; k <= nt; k++) {
        int t = (int)r_odf.getInt(section, num3("Tremulant", (int)k), 0);

        if (t >= 1 && t <= (int)r_organ.tremulants.size())
          w.tremulants.push_back(t - 1);
      }
      r_organ.windchests.push_back(w);
      m_windchestLevels.push_back(readLevel(section, ""));
    }
    if (r_organ.windchests.empty()) {
      r_organ.windchests.push_back(Windchest());
      m_windchestLevels.push_back(Level());
    }

    // shared ranks
    long nRanks = r_odf.getInt("Organ", "NumberOfRanks", 0);

    for (long i = 1; i <= nRanks; i++) {
      std::string section = num3("Rank", (int)i);

      loadRank(section, (int)r_odf.getInt(section, "NumberOfLogicalPipes", 0));
    }

    // manuals: first create them all, so couplers can find their destination
    bool hasPedals = r_odf.getBool("Organ", "HasPedals", false);
    long nManuals = r_odf.getInt("Organ", "NumberOfManuals", 0);

    for (long m = hasPedals ? 0 : 1; m <= nManuals; m++) {
      std::string section = num3("Manual", (int)m);
      Manual man;

      man.name = toUtf8(r_odf.get(section, "Name", section));
      man.odfNumber = (int)m;
      man.numLogical = (int)r_odf.getInt(section, "NumberOfLogicalKeys", 61);
      man.firstLogicalKey = (int)r_odf.getInt(section, "FirstAccessibleKeyLogicalKeyNumber", 1);
      man.firstMidiNote = (int)r_odf.getInt(section, "FirstAccessibleKeyMIDINoteNumber", 36);
      man.numAccessible = (int)r_odf.getInt(section, "NumberOfAccessibleKeys", man.numLogical);
      m_manualByOdf[(int)m] = (int)r_organ.manuals.size();
      r_organ.manuals.push_back(man);
    }
    for (size_t mi = 0; mi < r_organ.manuals.size(); mi++) {
      std::string section = num3("Manual", r_organ.manuals[mi].odfNumber);
      long ns = r_odf.getInt(section, "NumberOfStops", 0);
      long nc = r_odf.getInt(section, "NumberOfCouplers", 0);
      long nsw = r_odf.getInt(section, "NumberOfSwitches", 0);
      long nt = r_odf.getInt(section, "NumberOfTremulants", 0);

      for (long i = 1; i <= ns; i++) {
        int number = (int)r_odf.getInt(section, num3("Stop", (int)i), 0);

        if (number > 0) {
          int stopId = loadStop(number, (int)mi);

          r_organ.manuals[mi].stops.push_back(stopId);
          r_organ.manuals[mi].drawstops.push_back(r_organ.stops[(size_t)stopId].drawstop);
        }
      }
      for (long i = 1; i <= nc; i++) {
        int number = (int)r_odf.getInt(section, num3("Coupler", (int)i), 0);

        if (number > 0) {
          int couplerId = loadCoupler(number, (int)mi);

          r_organ.manuals[mi].couplers.push_back(couplerId);
          r_organ.manuals[mi].drawstops.push_back(r_organ.couplers[(size_t)couplerId].drawstop);
        }
      }
      for (long i = 1; i <= nsw; i++) {
        auto it = m_switchDrawstop.find((int)r_odf.getInt(section, num3("Switch", (int)i), 0));

        if (it != m_switchDrawstop.end()) {
          r_organ.manuals[mi].drawstops.push_back(it->second);
          if (r_organ.drawstops[(size_t)it->second].manual < 0)
            r_organ.drawstops[(size_t)it->second].manual = (int)mi;
        }
      }
      for (long i = 1; i <= nt; i++) {
        auto it = m_tremulantByNumber.find((int)r_odf.getInt(section, num3("Tremulant", (int)i), 0));

        if (it != m_tremulantByNumber.end()) {
          int ds = r_organ.tremulants[(size_t)it->second].drawstop;

          r_organ.manuals[mi].drawstops.push_back(ds);
          if (r_organ.drawstops[(size_t)ds].manual < 0)
            r_organ.drawstops[(size_t)ds].manual = (int)mi;
        }
      }
    }
    for (const Stop &s : r_organ.stops)
      r_organ.drawstops[(size_t)s.drawstop].manual = s.manual;
    for (const Coupler &c : r_organ.couplers)
      r_organ.drawstops[(size_t)c.drawstop].manual = c.manual;

    // logic inputs: switch numbers to drawstop ids
    for (size_t d = 0; d < r_organ.drawstops.size(); d++)
      for (int sw : m_rawInputs[d]) {
        auto it = m_switchDrawstop.find(sw);

        if (it != m_switchDrawstop.end())
          r_organ.drawstops[d].inputs.push_back(it->second);
      }

    loadGenerals();
    isOk = resolveRefs(err);
  }
  if (isOk)
    isOk = decodeSamples(progress, err);

  // a pipe whose samples all failed to decode is silenced
  if (isOk)
    for (Pipe &p : r_organ.pipes)
      p.attacks.erase(
        std::remove_if(
          p.attacks.begin(),
          p.attacks.end(),
          [&](const AttackInfo &a) { return r_organ.samples[(size_t)a.sample].frames == 0; }),
        p.attacks.end());
  return isOk;
}

} // namespace

bool loadOrgan(
  FileSource &source,
  const std::string &odfPath,
  Organ &organ,
  const std::function<void(float)> &progress,
  std::string &err) {
  std::vector<uint8_t> bytes;
  bool isOk = source.read(odfPath, bytes);

  if (!isOk)
    err = "Cannot read " + odfPath;
  if (isOk) {
    OdfFile odf;
    std::string path = normalizePath(odfPath);
    size_t slash = path.rfind('/');
    std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
    Loader loader(odf, source, organ, dir);

    odf.parse(std::string(bytes.begin(), bytes.end()));
    isOk = loader.run(progress, err);
  }
  return isOk;
}

} // namespace organ
