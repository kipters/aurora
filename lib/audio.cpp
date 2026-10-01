#include <aurora/audio.h>

#ifdef AURORA_UWP
#include "uwp/audio.hpp"
#endif

extern "C" {
SDL_AudioStream* aurora_open_playback_stream(const SDL_AudioSpec* spec, SDL_AudioStreamCallback callback,
                                             void* userdata) {
#ifdef AURORA_UWP
  return aurora::uwp::audio::open_playback_stream(spec, callback, userdata);
#else
  return SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, spec, callback, userdata);
#endif
}

bool aurora_pause_playback_stream(SDL_AudioStream* stream) {
#ifdef AURORA_UWP
  return aurora::uwp::audio::set_paused(stream, true);
#else
  return SDL_PauseAudioStreamDevice(stream);
#endif
}

bool aurora_resume_playback_stream(SDL_AudioStream* stream) {
#ifdef AURORA_UWP
  return aurora::uwp::audio::set_paused(stream, false);
#else
  return SDL_ResumeAudioStreamDevice(stream);
#endif
}

void aurora_destroy_playback_stream(SDL_AudioStream* stream) {
#ifdef AURORA_UWP
  aurora::uwp::audio::destroy_playback_stream(stream);
#else
  SDL_DestroyAudioStream(stream);
#endif
}
}
