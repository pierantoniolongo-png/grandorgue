// Low-latency audio output with AAudio.
// License GPL-2.0 or later.
#pragma once

#include "engine/engine.h"

/** Opens the output stream (idempotent); returns false on failure */
bool audioStart(organ::Engine &engine);
void audioStop();
int audioSampleRate();
int audioBufferFrames();
