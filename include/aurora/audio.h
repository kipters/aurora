#ifndef AURORA_AUDIO_H
#define AURORA_AUDIO_H

#include <SDL3/SDL_audio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Audio output on the default playback device.
//
// These behave like SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, ...) and the
// matching SDL_PauseAudioStreamDevice/SDL_ResumeAudioStreamDevice/SDL_DestroyAudioStream calls,
// and simply forward to them on most platforms. On UWP, where SDL has no audio backend, aurora
// renders the stream through WASAPI itself. The stream starts paused.
SDL_AudioStream* aurora_open_playback_stream(const SDL_AudioSpec* spec, SDL_AudioStreamCallback callback,
                                             void* userdata);
bool aurora_pause_playback_stream(SDL_AudioStream* stream);
bool aurora_resume_playback_stream(SDL_AudioStream* stream);
void aurora_destroy_playback_stream(SDL_AudioStream* stream);

#ifdef __cplusplus
}
#endif

#endif
