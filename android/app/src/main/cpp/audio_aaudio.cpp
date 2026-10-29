// Low-latency audio output with AAudio.
// License GPL-2.0 or later.
#include "audio_aaudio.h"

#include <aaudio/AAudio.h>
#include <android/log.h>

#include <atomic>
#include <mutex>
#include <thread>

#define LOG_TAG "OrganAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

organ::Engine *g_engine = nullptr;
AAudioStream *g_stream = nullptr;
std::mutex g_mutex;
std::atomic<bool> g_isWanted{false};
std::atomic<int> g_rate{0};
std::atomic<int> g_bufferFrames{0};

aaudio_data_callback_result_t data_callback(
  AAudioStream *, void *, void *audioData, int32_t numFrames) {
  g_engine->render(static_cast<float *>(audioData), numFrames);
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool open_stream_locked();

void error_callback(AAudioStream *, void *, aaudio_result_t error) {
  // the device changed (headphones, USB audio...): reopen from another thread
  if (error == AAUDIO_ERROR_DISCONNECTED && g_isWanted.load()) {
    std::thread([]() {
      std::lock_guard<std::mutex> lock(g_mutex);

      if (g_stream) {
        AAudioStream_requestStop(g_stream);
        AAudioStream_close(g_stream);
        g_stream = nullptr;
      }
      if (g_isWanted.load())
        open_stream_locked();
    }).detach();
  }
}

bool open_stream_locked() {
  AAudioStreamBuilder *builder = nullptr;
  bool isOk = AAudio_createStreamBuilder(&builder) == AAUDIO_OK;

  if (isOk) {
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setDataCallback(builder, data_callback, nullptr);
    AAudioStreamBuilder_setErrorCallback(builder, error_callback, nullptr);
    isOk = AAudioStreamBuilder_openStream(builder, &g_stream) == AAUDIO_OK;
    AAudioStreamBuilder_delete(builder);
  }
  if (isOk) {
    int32_t burst = AAudioStream_getFramesPerBurst(g_stream);

    // two bursts: low latency with a little safety margin
    AAudioStream_setBufferSizeInFrames(g_stream, burst * 2);
    g_rate.store(AAudioStream_getSampleRate(g_stream));
    g_bufferFrames.store(AAudioStream_getBufferSizeInFrames(g_stream));
    g_engine->setOutputRate(g_rate.load());
    isOk = AAudioStream_requestStart(g_stream) == AAUDIO_OK;
    LOGI("stream rate %d burst %d buffer %d", g_rate.load(), burst, g_bufferFrames.load());
  }
  return isOk;
}

} // namespace

bool audioStart(organ::Engine &engine) {
  std::lock_guard<std::mutex> lock(g_mutex);
  bool isOk = g_stream != nullptr;

  g_engine = &engine;
  g_isWanted.store(true);
  if (!isOk)
    isOk = open_stream_locked();
  return isOk;
}

void audioStop() {
  std::lock_guard<std::mutex> lock(g_mutex);

  g_isWanted.store(false);
  if (g_stream) {
    AAudioStream_requestStop(g_stream);
    AAudioStream_close(g_stream);
    g_stream = nullptr;
  }
}

int audioSampleRate() { return g_rate.load(); }

int audioBufferFrames() { return g_bufferFrames.load(); }
