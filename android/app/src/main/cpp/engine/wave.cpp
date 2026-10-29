// Decoding of pipe samples: RIFF WAVE and WavPack, with loops and cue point.
// License GPL-2.0 or later.
#include "wave.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <wavpack.h>

namespace organ {

static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t clamp16(int32_t v) {
  return (int16_t)std::max(-32768, std::min(32767, v));
}

struct WavFormat {
  uint16_t tag = 0;
  uint16_t channels = 0;
  uint32_t rate = 0;
  uint16_t bits = 0;
  uint16_t blockAlign = 0;
};

/**
 * Walks RIFF chunks. With isWrapper the buffer is a WavPack wrapper: the
 * 'data' chunk header is present but its payload is not.
 */
static void parse_chunks(
  const uint8_t *p,
  size_t len,
  bool isWrapper,
  WavFormat &fmt,
  const uint8_t **ppData,
  uint32_t *pDataLen,
  SampleData &out) {
  size_t pos = 12; // after "RIFF....WAVE"

  while (pos + 8 <= len) {
    const uint8_t *c = p + pos;
    uint32_t size = le32(c + 4);
    const uint8_t *body = c + 8;
    size_t avail = len - pos - 8;

    if (!std::memcmp(c, "fmt ", 4) && avail >= 16) {
      fmt.tag = le16(body);
      fmt.channels = le16(body + 2);
      fmt.rate = le32(body + 4);
      fmt.blockAlign = le16(body + 12);
      fmt.bits = le16(body + 14);
      if (fmt.tag == 0xFFFE && size >= 40 && avail >= 40)
        fmt.tag = le16(body + 24); // subformat of WAVE_FORMAT_EXTENSIBLE
    } else if (!std::memcmp(c, "smpl", 4) && avail >= 36) {
      uint32_t unity = le32(body + 12);
      uint32_t nLoops = le32(body + 28);

      out.midiUnityNote = unity < 128 ? (int)unity : -1;
      for (uint32_t i = 0; i < nLoops && 36 + 24 * (i + 1) <= avail; i++) {
        const uint8_t *l = body + 36 + 24 * i;
        Loop loop{le32(l + 8), le32(l + 12)};

        if (loop.end > loop.start)
          out.loops.push_back(loop);
      }
    } else if (!std::memcmp(c, "cue ", 4) && avail >= 4) {
      uint32_t nCues = le32(body);

      // GrandOrgue uses the last cue point as the release marker
      for (uint32_t i = 0; i < nCues && 4 + 24 * (i + 1) <= avail; i++)
        out.cue = le32(body + 4 + 24 * i + 20);
    } else if (!std::memcmp(c, "data", 4)) {
      if (isWrapper) {
        pos += 8; // the payload is not in the wrapper
        continue;
      }
      *ppData = body;
      *pDataLen = (uint32_t)std::min<size_t>(size, avail);
    }
    pos += 8 + size + (size & 1);
  }
}

static bool decode_wav(
  const std::vector<uint8_t> &bytes, SampleData &out, std::string &err) {
  WavFormat fmt;
  const uint8_t *pData = nullptr;
  uint32_t dataLen = 0;
  bool isOk = true;

  parse_chunks(bytes.data(), bytes.size(), false, fmt, &pData, &dataLen, out);
  if (!pData || !fmt.channels || !fmt.rate || !fmt.blockAlign) {
    err = "invalid WAV file";
    isOk = false;
  }
  if (isOk && !(fmt.tag == 1 || (fmt.tag == 3 && fmt.bits == 32))) {
    err = "unsupported WAV encoding";
    isOk = false;
  }
  if (isOk) {
    const int outCh = std::min<int>(fmt.channels, 2);
    const uint32_t frames = dataLen / fmt.blockAlign;
    const int bytesPerSample = fmt.bits / 8;

    out.channels = outCh;
    out.rate = (int)fmt.rate;
    out.frames = frames;
    out.pcm.resize((size_t)frames * outCh);
    for (uint32_t f = 0; f < frames; f++) {
      for (int ch = 0; ch < outCh; ch++) {
        const uint8_t *s = pData + (size_t)f * fmt.blockAlign + ch * bytesPerSample;
        int32_t v = 0;

        if (fmt.tag == 3) {
          float fv;

          std::memcpy(&fv, s, 4);
          v = (int32_t)std::lrint(fv * 32767.0f);
        } else if (fmt.bits == 8)
          v = ((int32_t)s[0] - 128) << 8;
        else if (fmt.bits == 16)
          v = (int16_t)le16(s);
        else if (fmt.bits == 24)
          v = ((int32_t)((s[0] << 8) | (s[1] << 16) | ((uint32_t)s[2] << 24))) >> 16;
        else if (fmt.bits == 32)
          v = ((int32_t)le32(s)) >> 16;
        out.pcm[(size_t)f * outCh + ch] = clamp16(v);
      }
    }
  }
  return isOk;
}

/*
 * WavPack from memory
 */

struct MemReader {
  const uint8_t *data;
  int64_t size;
  int64_t pos;
  int pushed;
};

static int32_t mr_read(void *id, void *buf, int32_t bcount) {
  MemReader *m = (MemReader *)id;
  uint8_t *out = (uint8_t *)buf;
  int32_t n = 0;

  if (bcount > 0 && m->pushed >= 0) {
    out[n++] = (uint8_t)m->pushed;
    m->pushed = -1;
  }

  int64_t avail = m->size - m->pos;
  int32_t take = (int32_t)std::min<int64_t>(avail, bcount - n);

  if (take > 0) {
    std::memcpy(out + n, m->data + m->pos, (size_t)take);
    m->pos += take;
    n += take;
  }
  return n;
}
static int32_t mr_write(void *, void *, int32_t) { return 0; }
static int64_t mr_get_pos(void *id) {
  MemReader *m = (MemReader *)id;
  return m->pos - (m->pushed >= 0 ? 1 : 0);
}
static int mr_set_pos_abs(void *id, int64_t pos) {
  MemReader *m = (MemReader *)id;
  m->pushed = -1;
  m->pos = std::max<int64_t>(0, std::min(pos, m->size));
  return 0;
}
static int mr_set_pos_rel(void *id, int64_t delta, int mode) {
  MemReader *m = (MemReader *)id;
  int64_t base = mode == SEEK_SET ? 0 : mode == SEEK_END ? m->size : mr_get_pos(id);
  return mr_set_pos_abs(id, base + delta);
}
static int mr_push_back(void *id, int c) {
  MemReader *m = (MemReader *)id;
  m->pushed = c;
  return c;
}
static int64_t mr_get_length(void *id) { return ((MemReader *)id)->size; }
static int mr_can_seek(void *) { return 1; }
static int mr_truncate(void *) { return 0; }
static int mr_close(void *) { return 0; }

static bool decode_wavpack(
  const std::vector<uint8_t> &bytes, SampleData &out, std::string &err) {
  static WavpackStreamReader64 reader = {
    mr_read,
    mr_write,
    mr_get_pos,
    mr_set_pos_abs,
    mr_set_pos_rel,
    mr_push_back,
    mr_get_length,
    mr_can_seek,
    mr_truncate,
    mr_close};
  MemReader mem{bytes.data(), (int64_t)bytes.size(), 0, -1};
  char wperr[80] = {0};
  WavpackContext *wpc = WavpackOpenFileInputEx64(
    &reader, &mem, nullptr, wperr, OPEN_WRAPPER | OPEN_DSD_AS_PCM, 0);
  bool isOk = wpc != nullptr;

  if (!isOk)
    err = std::string("WavPack: ") + wperr;
  if (isOk) {
    const int ch = WavpackGetNumChannels(wpc);
    const int outCh = std::min(ch, 2);
    const int64_t frames = WavpackGetNumSamples64(wpc);
    const int bits = WavpackGetBitsPerSample(wpc);
    const bool isFloat = (WavpackGetMode(wpc) & MODE_FLOAT) != 0;

    isOk = frames > 0 && ch > 0;
    if (!isOk)
      err = "WavPack: unknown length";
    if (isOk) {
      std::vector<int32_t> buf(4096 * (size_t)ch);
      int64_t done = 0;

      out.channels = outCh;
      out.rate = (int)WavpackGetSampleRate(wpc);
      out.frames = (uint32_t)frames;
      out.pcm.resize((size_t)frames * outCh);
      while (done < frames) {
        uint32_t got = WavpackUnpackSamples(wpc, buf.data(), 4096);

        if (!got)
          break;
        for (uint32_t f = 0; f < got && done + f < frames; f++)
          for (int c = 0; c < outCh; c++) {
            int32_t raw = buf[(size_t)f * ch + c];
            int32_t v;

            if (isFloat) {
              float fv;

              std::memcpy(&fv, &raw, 4);
              v = (int32_t)std::lrint(fv * 32767.0f);
            } else
              v = bits > 16 ? raw >> (bits - 16) : raw << (16 - bits);
            out.pcm[(size_t)(done + f) * outCh + c] = clamp16(v);
          }
        done += got;
      }

      // the RIFF header (and trailer) kept by WavPack holds smpl and cue
      WavpackSeekTrailingWrapper(wpc);

      uint32_t wrapLen = WavpackGetWrapperBytes(wpc);
      const uint8_t *wrap = WavpackGetWrapperData(wpc);

      if (wrap && wrapLen > 12 && !std::memcmp(wrap, "RIFF", 4)) {
        WavFormat fmt;
        const uint8_t *pData = nullptr;
        uint32_t dataLen = 0;

        parse_chunks(wrap, wrapLen, true, fmt, &pData, &dataLen, out);
      }
    }
    WavpackCloseFile(wpc);
  }
  return isOk;
}

bool decodeAudio(
  const std::vector<uint8_t> &bytes, SampleData &out, std::string &err) {
  bool isOk = false;

  if (bytes.size() >= 12 && !std::memcmp(bytes.data(), "RIFF", 4))
    isOk = decode_wav(bytes, out, err);
  else if (bytes.size() >= 32 && !std::memcmp(bytes.data(), "wvpk", 4))
    isOk = decode_wavpack(bytes, out, err);
  else
    err = "unknown audio format";

  if (isOk) {
    // drop loops and cue points outside the sample
    out.loops.erase(
      std::remove_if(
        out.loops.begin(),
        out.loops.end(),
        [&](const Loop &l) { return l.end >= out.frames; }),
      out.loops.end());
    if (out.cue >= (int64_t)out.frames)
      out.cue = -1;
  }
  return isOk;
}

} // namespace organ
