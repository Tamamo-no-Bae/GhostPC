#pragma once
#include <xaudio2.h>
#include <algorithm>
#include <math.h>
extern IXAudio2 *g_xa;
extern IXAudio2MasteringVoice *g_master;
bool AudioInit();
size_t XboxAdpcmDecode(const uint8_t *src, size_t bytes, int ch, short *out);
size_t XboxAdpcmFrames(size_t bytes, int ch);
