// Real-time organ engine: commands from any thread, rendering on the audio
// thread.
// License GPL-2.0 or later.
#include "engine.h"
#include "odf.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>

namespace organ {

namespace {

constexpr int MAX_VOICES = 512;
constexpr int BLOCK = 64;
constexpr double TWO_PI = 6.283185307179586;

std::string json_escape(const std::string &s) {
  std::string r;

  for (unsigned char c : s) {
    switch (c) {
    case '"':
      r += "\\\"";
      break;
    case '\\':
      r += "\\\\";
      break;
    case '\n':
      r += "\\n";
      break;
    case '\r':
    case '\t':
      r += ' ';
      break;
    default:
      if (c < 0x20)
        r += ' ';
      else
        r += (char)c;
    }
  }
  return r;
}

const char *kind_name(DrawstopKind k) {
  switch (k) {
  case DrawstopKind::STOP:
    return "stop";
  case DrawstopKind::COUPLER:
    return "coupler";
  case DrawstopKind::TREMULANT:
    return "tremulant";
  default:
    return "switch";
  }
}

struct Voice {
  bool isActive = false;
  const SampleData *s = nullptr;
  double pos = 0.0;
  double step = 1.0;
  bool isLooping = false;
  uint32_t loopStart = 0;
  uint32_t loopEnd = 0;
  uint32_t endFrame = 0;
  float gain = 0.0f;
  int windchest = 0;
  float env = 1.0f;
  float envStep = 0.0f; // per frame; negative while fading out
  int pipe = -1;
  uint64_t startTime = 0;
};

} // namespace

/** Playing state of a loaded organ; used only by the audio thread */
class Runtime {
public:
  explicit Runtime(std::unique_ptr<Organ> organ);

  const Organ &organ() const { return *m_organ; }
  std::atomic<uint8_t> *snapshot() const { return m_snapshot.get(); }

  void note(int manual, int note, bool isOn);
  void drawstop(int id, bool isOn);
  void enclosure(int id, int value);
  void general(int id);
  void generalCancel();
  void allOff();

  void render(float *out, int frames, int outRate, float master, int &nVoices);

private:
  std::unique_ptr<Organ> m_organ;
  int m_outRate = 48000;

  struct ManualState {
    std::vector<uint8_t> own;
    std::vector<uint8_t> eff;
    std::vector<int16_t> coupled;
    int unisonOff = 0;
  };
  std::vector<ManualState> m_manuals;
  std::vector<std::vector<uint8_t>> m_stopKeys;
  std::vector<uint8_t> m_state;   // logical drawstop states
  std::vector<uint8_t> m_applied; // states whose effects have been applied
  std::unique_ptr<std::atomic<uint8_t>[]> m_snapshot;

  std::vector<int16_t> m_pipeDemand;
  std::vector<int> m_pipeVoice;
  std::vector<uint64_t> m_pipeStart;

  std::vector<int> m_enclosureValue;
  std::vector<double> m_tremPhase;
  std::vector<float> m_tremDepth;
  std::vector<float> m_wcGain;

  std::vector<Voice> m_voices;
  int m_voiceHint = 0;
  uint64_t m_now = 0;
  uint32_t m_rng = 0x12345678u;

  uint32_t random() {
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return m_rng;
  }

  void evaluate();
  void applyDrawstop(int id, bool isOn);
  void setOwn(int manual, int key, bool isOn);
  void coupleKey(int manual, int key, bool isOn);
  void refreshKey(int manual, int key);
  void stopKey(int stop, int key, bool isOn);
  void setRankKey(int stop, int keyIndex, bool isOn);
  void pipeDemand(int pipe, bool isOn);
  bool windchestTremulantOn(int windchest) const;
  int allocVoice();
  void startPipe(int pipe);
  void releasePipe(int pipe);
  void startVoice(
    const SampleData *s, int pipe, int64_t startFrame, bool isLooping,
    const std::vector<Loop> &loops, float envStart, float envStep);
};

Runtime::Runtime(std::unique_ptr<Organ> organ) : m_organ(std::move(organ)) {
  const Organ &o = *m_organ;

  m_manuals.resize(o.manuals.size());
  for (size_t m = 0; m < o.manuals.size(); m++) {
    size_t n = (size_t)std::max(1, o.manuals[m].numLogical);

    m_manuals[m].own.assign(n, 0);
    m_manuals[m].eff.assign(n, 0);
    m_manuals[m].coupled.assign(n, 0);
  }
  m_stopKeys.resize(o.stops.size());
  for (size_t s = 0; s < o.stops.size(); s++)
    m_stopKeys[s].assign((size_t)std::max(1, o.stops[s].numKeys), 0);
  m_state.assign(o.drawstops.size(), 0);
  m_applied.assign(o.drawstops.size(), 0);
  m_snapshot.reset(new std::atomic<uint8_t>[std::max<size_t>(1, o.drawstops.size())]);
  m_pipeDemand.assign(o.pipes.size(), 0);
  m_pipeVoice.assign(o.pipes.size(), -1);
  m_pipeStart.assign(o.pipes.size(), 0);
  m_enclosureValue.assign(o.enclosures.size(), 127);
  m_tremPhase.assign(o.tremulants.size(), 0.0);
  m_tremDepth.assign(o.tremulants.size(), 0.0f);
  m_wcGain.assign(o.windchests.size(), 1.0f);
  m_voices.resize(MAX_VOICES);
  for (size_t d = 0; d < o.drawstops.size(); d++)
    if (o.drawstops[d].func == DrawstopFunc::INPUT)
      m_state[d] = o.drawstops[d].isDefaultOn ? 1 : 0;
  evaluate();
}

void Runtime::evaluate() {
  const Organ &o = *m_organ;
  bool isChanged = true;

  for (int pass = 0; pass < 16 && isChanged; pass++) {
    isChanged = false;
    for (size_t d = 0; d < o.drawstops.size(); d++) {
      const Drawstop &ds = o.drawstops[d];

      if (ds.func != DrawstopFunc::INPUT) {
        int nOn = 0;

        for (int in : ds.inputs)
          nOn += m_state[(size_t)in] ? 1 : 0;

        bool isAll = nOn == (int)ds.inputs.size() && !ds.inputs.empty();
        bool isAny = nOn > 0;
        bool v = ds.func == DrawstopFunc::AND ? isAll
          : ds.func == DrawstopFunc::OR       ? isAny
          : ds.func == DrawstopFunc::NOT      ? !isAny
          : ds.func == DrawstopFunc::NAND     ? !isAll
          : ds.func == DrawstopFunc::NOR      ? !isAny
                                              : (nOn % 2) == 1;

        if (m_state[d] != (v ? 1 : 0)) {
          m_state[d] = v ? 1 : 0;
          isChanged = true;
        }
      }
    }
  }
  for (size_t d = 0; d < o.drawstops.size(); d++) {
    if (m_applied[d] != m_state[d]) {
      m_applied[d] = m_state[d];
      applyDrawstop((int)d, m_state[d] != 0);
    }
    m_snapshot[d].store(m_state[d], std::memory_order_relaxed);
  }
}

void Runtime::applyDrawstop(int id, bool isOn) {
  const Organ &o = *m_organ;
  const Drawstop &ds = o.drawstops[(size_t)id];

  if (ds.kind == DrawstopKind::STOP) {
    const Stop &s = o.stops[(size_t)ds.target];

    if (s.isEffects) {
      const Rank &r = o.ranks[(size_t)s.ranks[0].rank];

      if (!r.pipes.empty() && r.pipes[0] >= 0)
        pipeDemand(r.pipes[0], isOn);
    } else {
      const std::vector<uint8_t> &keys = m_stopKeys[(size_t)ds.target];

      for (size_t k = 0; k < keys.size(); k++)
        if (keys[k])
          setRankKey(ds.target, (int)k, isOn);
    }
  } else if (ds.kind == DrawstopKind::COUPLER) {
    const Coupler &c = o.couplers[(size_t)ds.target];
    ManualState &src = m_manuals[(size_t)c.manual];

    if (c.isUnisonOff) {
      src.unisonOff += isOn ? 1 : -1;
      for (size_t k = 0; k < src.own.size(); k++)
        refreshKey(c.manual, (int)k);
    } else if (c.dest >= 0) {
      for (size_t k = 0; k < src.own.size(); k++)
        if (src.own[k])
          coupleKey(c.dest, (int)k + c.keyshift, isOn);
    }
  }
}

void Runtime::setOwn(int manual, int key, bool isOn) {
  const Organ &o = *m_organ;
  ManualState &ms = m_manuals[(size_t)manual];

  if (ms.own[(size_t)key] != (isOn ? 1 : 0)) {
    ms.own[(size_t)key] = isOn ? 1 : 0;
    for (int ci : o.manuals[(size_t)manual].couplers) {
      const Coupler &c = o.couplers[(size_t)ci];

      if (m_applied[(size_t)c.drawstop] && !c.isUnisonOff && c.dest >= 0)
        coupleKey(c.dest, key + c.keyshift, isOn);
    }
    refreshKey(manual, key);
  }
}

void Runtime::coupleKey(int manual, int key, bool isOn) {
  ManualState &ms = m_manuals[(size_t)manual];

  if (key >= 0 && key < (int)ms.coupled.size()) {
    ms.coupled[(size_t)key] += isOn ? 1 : -1;
    refreshKey(manual, key);
  }
}

void Runtime::refreshKey(int manual, int key) {
  ManualState &ms = m_manuals[(size_t)manual];
  bool isEff = (ms.own[(size_t)key] && ms.unisonOff == 0) || ms.coupled[(size_t)key] > 0;

  if (ms.eff[(size_t)key] != (isEff ? 1 : 0)) {
    ms.eff[(size_t)key] = isEff ? 1 : 0;
    for (int s : m_organ->manuals[(size_t)manual].stops)
      stopKey(s, key, isEff);
  }
}

void Runtime::stopKey(int stop, int key, bool isOn) {
  const Stop &s = m_organ->stops[(size_t)stop];
  int idx = key - s.firstKey;

  if (!s.isEffects && idx >= 0 && idx < s.numKeys) {
    uint8_t &k = m_stopKeys[(size_t)stop][(size_t)idx];

    if (k != (isOn ? 1 : 0)) {
      k = isOn ? 1 : 0;
      if (m_applied[(size_t)s.drawstop])
        setRankKey(stop, idx, isOn);
    }
  }
}

void Runtime::setRankKey(int stop, int keyIndex, bool isOn) {
  const Organ &o = *m_organ;

  for (const RankRef &rr : o.stops[(size_t)stop].ranks) {
    if (keyIndex >= rr.firstKey && keyIndex < rr.firstKey + rr.pipeCount) {
      int p = keyIndex - rr.firstKey + rr.firstPipe;
      const Rank &rank = o.ranks[(size_t)rr.rank];

      if (p >= 0 && p < (int)rank.pipes.size() && rank.pipes[(size_t)p] >= 0)
        pipeDemand(rank.pipes[(size_t)p], isOn);
    }
  }
}

void Runtime::pipeDemand(int pipe, bool isOn) {
  int16_t &d = m_pipeDemand[(size_t)pipe];

  if (isOn) {
    if (++d == 1)
      startPipe(pipe);
  } else if (d > 0) {
    if (--d == 0)
      releasePipe(pipe);
  }
}

bool Runtime::windchestTremulantOn(int windchest) const {
  bool isOn = false;

  if (windchest >= 0 && windchest < (int)m_organ->windchests.size())
    for (int t : m_organ->windchests[(size_t)windchest].tremulants)
      isOn = isOn || m_applied[(size_t)m_organ->tremulants[(size_t)t].drawstop];
  return isOn;
}

int Runtime::allocVoice() {
  int found = -1;

  for (int i = 0; i < MAX_VOICES && found < 0; i++) {
    int v = (m_voiceHint + i) % MAX_VOICES;

    if (!m_voices[(size_t)v].isActive)
      found = v;
  }
  if (found < 0) {
    // steal the quietest fading voice, otherwise the oldest one
    float bestEnv = 2.0f;
    uint64_t oldest = UINT64_MAX;

    for (int v = 0; v < MAX_VOICES; v++) {
      const Voice &vc = m_voices[(size_t)v];

      if (vc.envStep < 0.0f && vc.env < bestEnv) {
        bestEnv = vc.env;
        found = v;
      }
    }
    for (int v = 0; found < 0 && v < MAX_VOICES; v++)
      if (m_voices[(size_t)v].startTime < oldest) {
        oldest = m_voices[(size_t)v].startTime;
        found = v;
      }

    Voice &victim = m_voices[(size_t)found];

    if (victim.pipe >= 0 && m_pipeVoice[(size_t)victim.pipe] == found)
      m_pipeVoice[(size_t)victim.pipe] = -1;
  }
  m_voiceHint = (found + 1) % MAX_VOICES;
  return found;
}

void Runtime::startVoice(
  const SampleData *s, int pipe, int64_t startFrame, bool isLooping,
  const std::vector<Loop> &loops, float envStart, float envStep) {
  const Pipe &p = m_organ->pipes[(size_t)pipe];
  int vi = allocVoice();
  Voice &v = m_voices[(size_t)vi];

  v.isActive = true;
  v.s = s;
  v.pos = (double)std::max<int64_t>(0, startFrame);
  v.step = (double)s->rate / (double)m_outRate * std::pow(2.0, p.tuningCents / 1200.0);
  v.isLooping = isLooping && !loops.empty();
  if (v.isLooping) {
    const Loop &l = loops[random() % loops.size()];

    v.loopStart = l.start;
    v.loopEnd = l.end;
  }
  v.endFrame = s->frames;
  v.gain = p.gain;
  v.windchest = p.windchest;
  v.env = envStart;
  v.envStep = envStep;
  v.pipe = pipe;
  v.startTime = m_now;
  if (v.isLooping || envStep >= 0.0f)
    m_pipeVoice[(size_t)pipe] = vi;
}

void Runtime::startPipe(int pipe) {
  const Organ &o = *m_organ;
  const Pipe &p = o.pipes[(size_t)pipe];
  int want = windchestTremulantOn(p.windchest) ? 1 : 0;
  std::vector<int> candidates;

  for (size_t i = 0; i < p.attacks.size(); i++)
    if (p.attacks[i].isTremulant < 0 || p.attacks[i].isTremulant == want)
      candidates.push_back((int)i);
  if (candidates.empty())
    for (size_t i = 0; i < p.attacks.size(); i++)
      candidates.push_back((int)i);
  if (!candidates.empty()) {
    const AttackInfo &a = p.attacks[(size_t)candidates[random() % candidates.size()]];
    const SampleData &s = o.samples[(size_t)a.sample];
    const std::vector<Loop> &loops = a.loops.empty() ? s.loops : a.loops;

    // a voice still sounding for this pipe fades out quickly
    int old = m_pipeVoice[(size_t)pipe];

    if (old >= 0 && m_voices[(size_t)old].isActive)
      m_voices[(size_t)old].envStep = -1.0f / (0.02f * m_outRate);
    startVoice(&s, pipe, 0, !p.isPercussive, loops, 1.0f, 0.0f);
    m_pipeStart[(size_t)pipe] = m_now;
  }
}

void Runtime::releasePipe(int pipe) {
  const Organ &o = *m_organ;
  const Pipe &p = o.pipes[(size_t)pipe];
  int vi = m_pipeVoice[(size_t)pipe];

  if (vi >= 0 && m_voices[(size_t)vi].isActive && !p.isPercussive) {
    Voice &v = m_voices[(size_t)vi];
    double heldMs = (double)(m_now - m_pipeStart[(size_t)pipe]) * 1000.0 / m_outRate;
    int want = windchestTremulantOn(p.windchest) ? 1 : 0;
    const ReleaseInfo *pRel = nullptr;

    // the release with the smallest MaxKeyPressTime still >= the held time
    for (const ReleaseInfo &r : p.releases) {
      bool isTremOk = r.isTremulant < 0 || r.isTremulant == want;
      bool isTimeOk = r.maxKeyPressTime < 0 || r.maxKeyPressTime >= heldMs;

      if (isTremOk && isTimeOk && o.samples[(size_t)r.sample].frames > 0) {
        bool isBetter = !pRel
          || (r.maxKeyPressTime >= 0
              && (pRel->maxKeyPressTime < 0 || r.maxKeyPressTime < pRel->maxKeyPressTime));

        if (isBetter)
          pRel = &r;
      }
    }

    // find the attack the voice plays to know its cue point
    int64_t cue = -1;
    int xfMs = 0;

    for (const AttackInfo &a : p.attacks)
      if (&o.samples[(size_t)a.sample] == v.s) {
        cue = a.cue == -1 ? v.s->cue : a.cue;
        xfMs = a.releaseCrossfadeMs;
      }

    static const std::vector<Loop> noLoops;

    if (pRel) {
      float xf = (float)(pRel->releaseCrossfadeMs > 0 ? pRel->releaseCrossfadeMs : 30);

      v.envStep = -1.0f / (xf * 0.001f * m_outRate);
      startVoice(&o.samples[(size_t)pRel->sample], pipe, 0, false, noLoops, 1.0f, 0.0f);
    } else if (cue >= 0 && cue < (int64_t)v.s->frames) {
      float xf = (float)(xfMs > 0 ? xfMs : 30);
      float stepPerFrame = 1.0f / (xf * 0.001f * m_outRate);

      v.envStep = -stepPerFrame;
      startVoice(v.s, pipe, cue, false, noLoops, 0.0f, stepPerFrame);
    } else
      v.envStep = -1.0f / (0.15f * m_outRate);
    m_pipeVoice[(size_t)pipe] = -1;
  } else if (vi >= 0 && p.isPercussive)
    m_pipeVoice[(size_t)pipe] = -1; // a percussive pipe plays to its end
}

void Runtime::note(int manual, int note, bool isOn) {
  if (manual >= 0 && manual < (int)m_manuals.size()) {
    const Manual &m = m_organ->manuals[(size_t)manual];
    int idx = note - m.firstMidiNote;
    int key = idx + m.firstLogicalKey - 1;

    if (idx >= 0 && idx < m.numAccessible && key >= 0
        && key < (int)m_manuals[(size_t)manual].own.size())
      setOwn(manual, key, isOn);
  }
}

void Runtime::drawstop(int id, bool isOn) {
  if (id >= 0 && id < (int)m_state.size()
      && m_organ->drawstops[(size_t)id].func == DrawstopFunc::INPUT) {
    m_state[(size_t)id] = isOn ? 1 : 0;
    evaluate();
  }
}

void Runtime::enclosure(int id, int value) {
  if (id >= 0 && id < (int)m_enclosureValue.size())
    m_enclosureValue[(size_t)id] = std::max(0, std::min(127, value));
}

void Runtime::general(int id) {
  if (id >= 0 && id < (int)m_organ->generals.size()) {
    for (const auto &setting : m_organ->generals[(size_t)id].settings)
      if (m_organ->drawstops[(size_t)setting.first].func == DrawstopFunc::INPUT)
        m_state[(size_t)setting.first] = setting.second ? 1 : 0;
    evaluate();
  }
}

void Runtime::generalCancel() {
  const Organ &o = *m_organ;

  for (size_t d = 0; d < o.drawstops.size(); d++) {
    const Drawstop &ds = o.drawstops[d];

    if (ds.func == DrawstopFunc::INPUT && ds.gcState >= 0)
      m_state[d] = ds.gcState == 1 ? 1 : 0;
  }
  evaluate();
}

void Runtime::allOff() {
  for (size_t m = 0; m < m_manuals.size(); m++)
    for (size_t k = 0; k < m_manuals[m].own.size(); k++)
      if (m_manuals[m].own[k])
        setOwn((int)m, (int)k, false);
}

void Runtime::render(float *out, int frames, int outRate, float master, int &nVoices) {
  const Organ &o = *m_organ;
  const size_t nWc = o.windchests.size();
  std::vector<float> &wcEnd = m_wcGain;
  float wcStart[256];

  m_outRate = outRate;
  std::memset(out, 0, sizeof(float) * 2 * (size_t)frames);
  for (int off = 0; off < frames; off += BLOCK) {
    const int n = std::min(BLOCK, frames - off);
    const float blockSec = (float)n / (float)outRate;
    float *dst = out + 2 * off;

    // tremulant depth ramps and phases
    for (size_t t = 0; t < o.tremulants.size(); t++) {
      const Tremulant &tr = o.tremulants[t];
      float target = m_applied[(size_t)tr.drawstop] ? (float)tr.depth : 0.0f;
      float maxStep = (float)tr.depth * blockSec / 0.3f + 1e-6f;
      float &d = m_tremDepth[t];

      d += std::max(-maxStep, std::min(maxStep, target - d));
      m_tremPhase[t] = std::fmod(m_tremPhase[t] + blockSec * 1000.0 / tr.periodMs, 1.0);
    }

    // windchest gains for this block
    for (size_t w = 0; w < nWc && w < 256; w++) {
      const Windchest &wc = o.windchests[w];
      float g = 1.0f;

      for (int e : wc.enclosures) {
        float minL = o.enclosures[(size_t)e].minLevel;

        g *= minL + (1.0f - minL) * (float)m_enclosureValue[(size_t)e] / 127.0f;
      }
      for (int t : wc.tremulants)
        g *= 1.0f - m_tremDepth[(size_t)t] * 0.5f * (1.0f - (float)std::cos(TWO_PI * m_tremPhase[(size_t)t]));
      wcStart[w] = wcEnd[w];
      wcEnd[w] = g;
    }

    for (int vi = 0; vi < MAX_VOICES; vi++) {
      Voice &v = m_voices[(size_t)vi];

      if (!v.isActive)
        continue;

      const int16_t *pcm = v.s->pcm.data();
      const int ch = v.s->channels;
      const float g0 = v.windchest < 256 ? wcStart[v.windchest] : 1.0f;
      const float g1 = v.windchest < 256 ? wcEnd[v.windchest] : 1.0f;
      const float gStep = (g1 - g0) / (float)n;
      float wg = g0;

      for (int i = 0; i < n; i++) {
        uint32_t idx = (uint32_t)v.pos;

        if (!v.isLooping && idx >= v.endFrame) {
          v.isActive = false;
          break;
        }

        float frac = (float)(v.pos - (double)idx);
        uint32_t idx1 = idx + 1;

        if (v.isLooping && idx1 > v.loopEnd)
          idx1 = v.loopStart;
        else if (idx1 >= v.endFrame)
          idx1 = idx;

        const int16_t *a = pcm + (size_t)idx * ch;
        const int16_t *b = pcm + (size_t)idx1 * ch;
        float l = (float)a[0] + ((float)b[0] - (float)a[0]) * frac;
        float r = ch == 2 ? (float)a[1] + ((float)b[1] - (float)a[1]) * frac : l;
        float g = v.gain * v.env * wg;

        dst[2 * i] += l * g;
        dst[2 * i + 1] += r * g;
        wg += gStep;
        if (v.envStep != 0.0f) {
          v.env += v.envStep;
          if (v.envStep < 0.0f && v.env <= 0.0f) {
            v.isActive = false;
            break;
          }
          if (v.envStep > 0.0f && v.env >= 1.0f) {
            v.env = 1.0f;
            v.envStep = 0.0f;
          }
        }
        v.pos += v.step;
        if (v.isLooping) {
          const double loopLen = (double)(v.loopEnd + 1 - v.loopStart);

          while (v.pos >= (double)v.loopEnd + 1.0)
            v.pos -= loopLen;
        }
      }
      if (!v.isActive && v.pipe >= 0 && m_pipeVoice[(size_t)v.pipe] == vi)
        m_pipeVoice[(size_t)v.pipe] = -1;
    }
    m_now += (uint64_t)n;
  }

  // master volume and a soft limiter above 0.8
  int active = 0;

  for (const Voice &v : m_voices)
    active += v.isActive ? 1 : 0;
  nVoices = active;
  for (int i = 0; i < 2 * frames; i++) {
    float x = out[i] * master;
    float ax = std::fabs(x);

    if (ax > 0.8f)
      x = std::copysign(0.8f + 0.2f * std::tanh((ax - 0.8f) / 0.2f), x);
    out[i] = x;
  }
}

/*
 * Engine
 */

Engine::Engine() : m_queue(QUEUE_SIZE) {}

Engine::~Engine() { unload(); }

std::string Engine::listOrgans(const std::string &path) {
  std::string err;
  std::unique_ptr<FileSource> src = FileSource::open(path, err);
  std::ostringstream os;

  os << "[";
  if (src) {
    bool isFirst = true;

    for (const std::string &name : src->listOrgans()) {
      os << (isFirst ? "" : ",") << "\"" << json_escape(toUtf8(name)) << "\"";
      isFirst = false;
    }
  }
  os << "]";
  return os.str();
}

bool Engine::load(const std::string &path, const std::string &odf, std::string &err) {
  std::lock_guard<std::mutex> lock(m_loadMutex);
  std::unique_ptr<FileSource> src = FileSource::open(path, err);
  bool isOk = src != nullptr;

  m_progress.store(0.0f);
  if (isOk) {
    std::string odfPath = odf;

    if (odfPath.empty()) {
      std::vector<std::string> organs = src->listOrgans();

      isOk = !organs.empty();
      if (isOk)
        odfPath = organs[0];
      else
        err = "No organ definition (.organ) found";
    }
    if (isOk) {
      // free the previous organ first: memory is precious on a phone
      Runtime *old = m_runtime.exchange(nullptr);

      while (m_isRendering.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      delete old;

      auto organ = std::make_unique<Organ>();

      isOk = loadOrgan(*src, odfPath, *organ, [this](float p) { m_progress.store(p); }, err);
      if (isOk) {
        Runtime *rt = new Runtime(std::move(organ));

        m_tail.store(m_head.load()); // drop commands meant for the old organ
        m_runtime.store(rt);
      }
    }
  }
  m_progress.store(1.0f);
  return isOk;
}

void Engine::unload() {
  std::lock_guard<std::mutex> lock(m_loadMutex);
  Runtime *old = m_runtime.exchange(nullptr);

  while (m_isRendering.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  delete old;
}

std::string Engine::describe() const {
  std::lock_guard<std::mutex> lock(m_loadMutex);
  Runtime *rt = m_runtime.load();
  std::ostringstream os;

  if (!rt)
    return "{}";

  const Organ &o = rt->organ();
  std::vector<uint8_t> isGrouped(o.drawstops.size(), 0);

  os << "{\"name\":\"" << json_escape(o.name) << "\",\"builder\":\"" << json_escape(o.builder)
     << "\",\"memoryMB\":" << (o.memoryBytes / (1024 * 1024)) << ",\"warnings\":[";
  for (size_t i = 0; i < o.warnings.size() && i < 20; i++)
    os << (i ? "," : "") << "\"" << json_escape(toUtf8(o.warnings[i])) << "\"";
  os << "],\"warningCount\":" << o.warnings.size() << ",\"manuals\":[";
  for (size_t m = 0; m < o.manuals.size(); m++) {
    const Manual &man = o.manuals[m];

    os << (m ? "," : "") << "{\"id\":" << m << ",\"name\":\"" << json_escape(man.name)
       << "\",\"odf\":" << man.odfNumber << ",\"firstNote\":" << man.firstMidiNote
       << ",\"keys\":" << man.numAccessible << ",\"drawstops\":[";

    bool isFirst = true;

    for (int d : man.drawstops)
      if (o.drawstops[(size_t)d].func == DrawstopFunc::INPUT && !isGrouped[(size_t)d]) {
        os << (isFirst ? "" : ",") << d;
        isGrouped[(size_t)d] = 1;
        isFirst = false;
      }
    os << "]}";
  }
  os << "],\"organDrawstops\":[";

  bool isFirst = true;

  for (size_t d = 0; d < o.drawstops.size(); d++)
    if (o.drawstops[d].func == DrawstopFunc::INPUT && !isGrouped[d]) {
      os << (isFirst ? "" : ",") << d;
      isFirst = false;
    }
  os << "],\"drawstops\":[";
  for (size_t d = 0; d < o.drawstops.size(); d++) {
    const Drawstop &ds = o.drawstops[d];

    os << (d ? "," : "") << "{\"id\":" << d << ",\"name\":\"" << json_escape(ds.name)
       << "\",\"kind\":\"" << kind_name(ds.kind) << "\",\"input\":"
       << (ds.func == DrawstopFunc::INPUT ? "true" : "false") << "}";
  }
  os << "],\"enclosures\":[";
  for (size_t e = 0; e < o.enclosures.size(); e++)
    os << (e ? "," : "") << "{\"id\":" << e << ",\"name\":\"" << json_escape(o.enclosures[e].name)
       << "\"}";
  os << "],\"generals\":[";
  for (size_t g = 0; g < o.generals.size(); g++)
    os << (g ? "," : "") << "{\"id\":" << g << ",\"name\":\"" << json_escape(o.generals[g].name)
       << "\"}";
  os << "]}";
  return os.str();
}

void Engine::push(const Cmd &cmd) {
  std::lock_guard<std::mutex> lock(m_pushMutex);
  size_t head = m_head.load(std::memory_order_relaxed);
  size_t next = (head + 1) % QUEUE_SIZE;

  if (next != m_tail.load(std::memory_order_acquire)) {
    m_queue[head] = cmd;
    m_head.store(next, std::memory_order_release);
  }
}

void Engine::noteOn(int manual, int note, int velocity) {
  push(Cmd{CmdType::NOTE, manual, note, (float)velocity});
}
void Engine::setDrawstop(int drawstop, bool isOn) {
  push(Cmd{CmdType::DRAWSTOP, drawstop, isOn ? 1 : 0, 0.0f});
}
void Engine::setEnclosure(int enclosure, int value) {
  push(Cmd{CmdType::ENCLOSURE, enclosure, value, 0.0f});
}
void Engine::general(int general) { push(Cmd{CmdType::GENERAL, general, 0, 0.0f}); }
void Engine::generalCancel() { push(Cmd{CmdType::GC, 0, 0, 0.0f}); }
void Engine::allNotesOff() { push(Cmd{CmdType::ALL_OFF, 0, 0, 0.0f}); }
void Engine::setMasterGain(float gain) { m_masterGain.store(gain); }

std::vector<uint8_t> Engine::drawstopStates() const {
  std::vector<uint8_t> states;
  std::unique_lock<std::mutex> lock(m_loadMutex, std::try_to_lock);

  if (lock.owns_lock()) {
    Runtime *rt = m_runtime.load();

    if (rt) {
      size_t n = rt->organ().drawstops.size();

      states.resize(n);
      for (size_t i = 0; i < n; i++)
        states[i] = rt->snapshot()[i].load(std::memory_order_relaxed);
    }
  }
  return states;
}

void Engine::render(float *out, int frames) {
  m_isRendering.store(true);

  Runtime *rt = m_runtime.load();
  size_t tail = m_tail.load(std::memory_order_relaxed);
  const size_t head = m_head.load(std::memory_order_acquire);

  while (tail != head) {
    const Cmd &c = m_queue[tail];

    if (rt) {
      switch (c.type) {
      case CmdType::NOTE:
        rt->note(c.a, c.b, c.f > 0.0f);
        break;
      case CmdType::DRAWSTOP:
        rt->drawstop(c.a, c.b != 0);
        break;
      case CmdType::ENCLOSURE:
        rt->enclosure(c.a, c.b);
        break;
      case CmdType::GENERAL:
        rt->general(c.a);
        break;
      case CmdType::GC:
        rt->generalCancel();
        break;
      case CmdType::ALL_OFF:
        rt->allOff();
        break;
      case CmdType::GAIN:
        break;
      }
    }
    tail = (tail + 1) % QUEUE_SIZE;
  }
  m_tail.store(tail, std::memory_order_release);

  if (rt) {
    int nVoices = 0;

    rt->render(out, frames, m_outputRate.load(), m_masterGain.load(), nVoices);
    m_activeVoices.store(nVoices);
  } else
    std::memset(out, 0, sizeof(float) * 2 * (size_t)frames);
  m_isRendering.store(false);
}

} // namespace organ
