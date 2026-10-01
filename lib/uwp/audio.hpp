#pragma once

#ifdef AURORA_UWP

#include <SDL3/SDL_audio.h>

// WASAPI playback for UWP, where SDL has no audio backend. A render thread pulls from an
// unbound SDL_AudioStream, so the app's stream callback and SDL's format conversion work
// exactly as they would with a real SDL audio device.
namespace aurora::uwp::audio {

SDL_AudioStream* open_playback_stream(const SDL_AudioSpec* spec, SDL_AudioStreamCallback callback,
                                      void* userdata) noexcept;
bool set_paused(SDL_AudioStream* stream, bool paused) noexcept;
void destroy_playback_stream(SDL_AudioStream* stream) noexcept;

} // namespace aurora::uwp::audio

#endif
