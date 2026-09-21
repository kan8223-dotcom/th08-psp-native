#pragma once
#if defined(PSP) && defined(TH08_PSP_ME_AUDIO_MIX) && TH08_PSP_ME_AUDIO_MIX
#define TH08_PSP_ME_AUDIO_ENABLED 1
#else
#define TH08_PSP_ME_AUDIO_ENABLED 0
#endif
#if TH08_PSP_ME_AUDIO_ENABLED
#include "me_audio_math.hpp"
namespace th08::psp
{
int MeAudioInit();
void MeAudioRequestStop(); // signal only, safe in the emergency exit path
void MeAudioShutdown();   // never releases ownership without STOP acknowledgement
bool MeAudioCanExit();    // lock-free watchdog gate; active=false is NOT an ACK
const char *MeAudioFeature();
// Single SDL callback owns Prepare/Add/Run. Main never touches its packet.
MeAudioJob *MeAudioPrepare(unsigned frames);
bool MeAudioAdd(MeAudioJob *, const MeAudioVoice &, const unsigned char *pcm);
bool MeAudioRun(MeAudioJob *, std::int16_t *output);
void MeAudioRelease();   // release callback ownership, including admission refusal
void MeAudioNoteSc(unsigned us);
}
#endif
