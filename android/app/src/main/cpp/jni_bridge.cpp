// JNI bridge between the Java user interface and the organ engine.
// License GPL-2.0 or later.
#include <jni.h>

#include <string>
#include <vector>

#include "audio_aaudio.h"
#include "engine/engine.h"

namespace {

organ::Engine &engine() {
  static organ::Engine instance;
  return instance;
}

std::string to_string(JNIEnv *env, jstring s) {
  std::string result;

  if (s) {
    const char *c = env->GetStringUTFChars(s, nullptr);

    result = c;
    env->ReleaseStringUTFChars(s, c);
  }
  return result;
}

// strings may contain any UTF-8: pass them as bytes, Java decodes them
jbyteArray to_bytes(JNIEnv *env, const std::string &s) {
  jbyteArray arr = env->NewByteArray((jsize)s.size());

  env->SetByteArrayRegion(arr, 0, (jsize)s.size(), (const jbyte *)s.data());
  return arr;
}

} // namespace

#define JNI_FN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_org_organovirtuale_app_NativeEngine_##name

JNI_FN(jbyteArray, listOrgans)(JNIEnv *env, jclass, jstring path) {
  return to_bytes(env, organ::Engine::listOrgans(to_string(env, path)));
}

JNI_FN(jbyteArray, load)(JNIEnv *env, jclass, jstring path, jstring odf) {
  std::string err;

  if (engine().load(to_string(env, path), to_string(env, odf), err))
    err.clear();
  else if (err.empty())
    err = "unknown error";
  return to_bytes(env, err);
}

JNI_FN(void, unload)(JNIEnv *, jclass) { engine().unload(); }

JNI_FN(jfloat, progress)(JNIEnv *, jclass) { return engine().loadProgress(); }

JNI_FN(jbyteArray, describe)(JNIEnv *env, jclass) {
  return to_bytes(env, engine().describe());
}

JNI_FN(void, noteOn)(JNIEnv *, jclass, jint manual, jint note, jint velocity) {
  engine().noteOn(manual, note, velocity);
}

JNI_FN(void, setDrawstop)(JNIEnv *, jclass, jint id, jboolean isOn) {
  engine().setDrawstop(id, isOn);
}

JNI_FN(void, setEnclosure)(JNIEnv *, jclass, jint id, jint value) {
  engine().setEnclosure(id, value);
}

JNI_FN(void, general)(JNIEnv *, jclass, jint id) { engine().general(id); }

JNI_FN(void, generalCancel)(JNIEnv *, jclass) { engine().generalCancel(); }

JNI_FN(void, allNotesOff)(JNIEnv *, jclass) { engine().allNotesOff(); }

JNI_FN(void, setMasterGain)(JNIEnv *, jclass, jfloat gain) { engine().setMasterGain(gain); }

JNI_FN(jbyteArray, drawstopStates)(JNIEnv *env, jclass) {
  std::vector<uint8_t> s = engine().drawstopStates();

  return to_bytes(env, std::string(s.begin(), s.end()));
}

JNI_FN(jint, activeVoices)(JNIEnv *, jclass) { return engine().activeVoices(); }

JNI_FN(jboolean, startAudio)(JNIEnv *, jclass) { return audioStart(engine()); }

JNI_FN(void, stopAudio)(JNIEnv *, jclass) { audioStop(); }

JNI_FN(jint, sampleRate)(JNIEnv *, jclass) { return audioSampleRate(); }

JNI_FN(jint, bufferFrames)(JNIEnv *, jclass) { return audioBufferFrames(); }
