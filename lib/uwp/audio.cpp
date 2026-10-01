#include "audio.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <unknwn.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
// C++/WinRT goes before aurora's headers, whose TRY macro collides with a WinRT method name.
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Devices.h>

#include "../internal.hpp"

#include <SDL3/SDL_error.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace aurora::uwp::audio {
namespace {
constexpr Module Log{"aurora::uwp::audio"};

// Shared-mode buffer length requested from WASAPI, in 100ns units.
constexpr REFERENCE_TIME BufferDuration = 20 * 10000;

struct ActivationHandler
: winrt::implements<ActivationHandler, IActivateAudioInterfaceCompletionHandler> {
  winrt::handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  winrt::com_ptr<IAudioClient> client;
  HRESULT result = E_FAIL;

  HRESULT __stdcall ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) noexcept override {
    winrt::com_ptr<IUnknown> unknown;
    HRESULT activateResult = E_FAIL;
    result = operation->GetActivateResult(&activateResult, unknown.put());
    if (SUCCEEDED(result)) {
      result = activateResult;
    }
    if (SUCCEEDED(result)) {
      client = unknown.try_as<IAudioClient>();
      if (!client) {
        result = E_NOINTERFACE;
      }
    }
    SetEvent(done.get());
    return S_OK;
  }
};

// UWP apps can't enumerate endpoints through IMMDeviceEnumerator; they activate the default
// render device by its interface ID instead.
winrt::com_ptr<IAudioClient> activate_default_client() {
  const auto deviceId = winrt::Windows::Media::Devices::MediaDevice::GetDefaultAudioRenderId(
      winrt::Windows::Media::Devices::AudioDeviceRole::Default);
  if (deviceId.empty()) {
    Log.warn("No default audio render device");
    return nullptr;
  }
  const auto handler = winrt::make_self<ActivationHandler>();
  winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> operation;
  HRESULT hr = ActivateAudioInterfaceAsync(deviceId.c_str(), __uuidof(IAudioClient), nullptr, handler.get(),
                                           operation.put());
  if (FAILED(hr)) {
    Log.error("ActivateAudioInterfaceAsync failed: {:#010x}", static_cast<uint32_t>(hr));
    return nullptr;
  }
  WaitForSingleObject(handler->done.get(), INFINITE);
  if (FAILED(handler->result)) {
    Log.error("Activating the audio device failed: {:#010x}", static_cast<uint32_t>(handler->result));
    return nullptr;
  }
  return handler->client;
}

bool spec_from_wave_format(const WAVEFORMATEX& format, SDL_AudioSpec& spec) {
  const bool isFloat =
      format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
      (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
       reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format).SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  const bool isPcm = format.wFormatTag == WAVE_FORMAT_PCM ||
                     (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                      reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format).SubFormat == KSDATAFORMAT_SUBTYPE_PCM);
  if (isFloat && format.wBitsPerSample == 32) {
    spec.format = SDL_AUDIO_F32;
  } else if (isPcm && format.wBitsPerSample == 16) {
    spec.format = SDL_AUDIO_S16;
  } else if (isPcm && format.wBitsPerSample == 32) {
    spec.format = SDL_AUDIO_S32;
  } else {
    return false;
  }
  spec.channels = format.nChannels;
  spec.freq = static_cast<int>(format.nSamplesPerSec);
  return true;
}

class PlaybackStream {
public:
  explicit PlaybackStream(SDL_AudioStream* stream) : m_stream(stream) {
    m_thread = std::thread([this] { render_thread(); });
  }

  ~PlaybackStream() {
    m_quit.store(true, std::memory_order_release);
    SetEvent(m_wake.get());
    m_thread.join();
  }

  PlaybackStream(const PlaybackStream&) = delete;
  PlaybackStream& operator=(const PlaybackStream&) = delete;

  void set_paused(bool paused) noexcept { m_paused.store(paused, std::memory_order_release); }

private:
  void render_thread() noexcept {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetThreadDescription(GetCurrentThread(), L"Audio output");
    // Reopen the device whenever it goes away, e.g. when the default output changes.
    while (!m_quit.load(std::memory_order_acquire)) {
      if (!render_until_invalidated()) {
        // Couldn't open a device. Try again later rather than spinning.
        WaitForSingleObject(m_wake.get(), 1000);
      }
    }
    winrt::uninit_apartment();
  }

  // Returns false if the device couldn't be opened.
  bool render_until_invalidated() noexcept {
    winrt::com_ptr<IAudioClient> client;
    try {
      client = activate_default_client();
    } catch (const winrt::hresult_error& e) {
      Log.error("Failed to open the audio device: {}", winrt::to_string(e.message()));
    }
    if (!client) {
      return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    HRESULT hr = client->GetMixFormat(&mixFormat);
    if (FAILED(hr)) {
      Log.error("IAudioClient::GetMixFormat failed: {:#010x}", static_cast<uint32_t>(hr));
      return false;
    }
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mixFormatOwner{mixFormat, &CoTaskMemFree};
    SDL_AudioSpec deviceSpec{};
    if (!spec_from_wave_format(*mixFormat, deviceSpec)) {
      Log.error("Unsupported audio mix format (tag {:#x}, {} bits)", mixFormat->wFormatTag, mixFormat->wBitsPerSample);
      return false;
    }
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, BufferDuration, 0, mixFormat,
                            nullptr);
    if (FAILED(hr)) {
      Log.error("IAudioClient::Initialize failed: {:#010x}", static_cast<uint32_t>(hr));
      return false;
    }
    winrt::handle bufferReady{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    UINT32 bufferFrames = 0;
    winrt::com_ptr<IAudioRenderClient> renderClient;
    if (FAILED(hr = client->SetEventHandle(bufferReady.get())) || FAILED(hr = client->GetBufferSize(&bufferFrames)) ||
        FAILED(hr = client->GetService(__uuidof(IAudioRenderClient), renderClient.put_void()))) {
      Log.error("Failed to set up the audio render client: {:#010x}", static_cast<uint32_t>(hr));
      return false;
    }
    if (!SDL_SetAudioStreamFormat(m_stream, nullptr, &deviceSpec)) {
      Log.error("Failed to set the audio stream output format: {}", SDL_GetError());
      return false;
    }
    Log.info("Audio output: {} Hz, {} channels, {} frame buffer", deviceSpec.freq, deviceSpec.channels, bufferFrames);

    const UINT32 frameSize = mixFormat->nBlockAlign;
    if (FAILED(hr = client->Start())) {
      Log.error("IAudioClient::Start failed: {:#010x}", static_cast<uint32_t>(hr));
      return false;
    }
    const HANDLE waitHandles[] = {bufferReady.get(), m_wake.get()};
    while (!m_quit.load(std::memory_order_acquire)) {
      WaitForMultipleObjects(2, waitHandles, FALSE, 200);
      UINT32 padding = 0;
      hr = client->GetCurrentPadding(&padding);
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        Log.info("Audio device changed, reopening");
        break;
      }
      if (FAILED(hr) || padding >= bufferFrames) {
        continue;
      }
      const UINT32 frames = bufferFrames - padding;
      BYTE* data = nullptr;
      hr = renderClient->GetBuffer(frames, &data);
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        Log.info("Audio device changed, reopening");
        break;
      }
      if (FAILED(hr)) {
        continue;
      }
      const int bytes = static_cast<int>(frames * frameSize);
      int written = 0;
      if (!m_paused.load(std::memory_order_acquire)) {
        written = std::max(SDL_GetAudioStreamData(m_stream, data, bytes), 0);
      }
      if (written < bytes) {
        std::memset(data + written, 0, bytes - written);
      }
      renderClient->ReleaseBuffer(frames, 0);
    }
    client->Stop();
    return true;
  }

  SDL_AudioStream* m_stream;
  winrt::handle m_wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  std::atomic_bool m_paused = true;
  std::atomic_bool m_quit = false;
  std::thread m_thread;
};

std::mutex g_streamsMutex;
std::unordered_map<SDL_AudioStream*, std::unique_ptr<PlaybackStream>> g_streams;
} // namespace

SDL_AudioStream* open_playback_stream(const SDL_AudioSpec* spec, SDL_AudioStreamCallback callback,
                                      void* userdata) noexcept {
  // The output format is set once the device's mix format is known.
  SDL_AudioStream* stream = SDL_CreateAudioStream(spec, spec);
  if (stream == nullptr) {
    return nullptr;
  }
  if (callback != nullptr && !SDL_SetAudioStreamGetCallback(stream, callback, userdata)) {
    SDL_DestroyAudioStream(stream);
    return nullptr;
  }
  std::lock_guard lock{g_streamsMutex};
  g_streams.emplace(stream, std::make_unique<PlaybackStream>(stream));
  return stream;
}

bool set_paused(SDL_AudioStream* stream, bool paused) noexcept {
  std::lock_guard lock{g_streamsMutex};
  const auto it = g_streams.find(stream);
  if (it == g_streams.end()) {
    return SDL_SetError("Not a playback stream");
  }
  it->second->set_paused(paused);
  return true;
}

void destroy_playback_stream(SDL_AudioStream* stream) noexcept {
  if (stream == nullptr) {
    return;
  }
  std::unique_ptr<PlaybackStream> playback;
  {
    std::lock_guard lock{g_streamsMutex};
    if (const auto it = g_streams.find(stream); it != g_streams.end()) {
      playback = std::move(it->second);
      g_streams.erase(it);
    }
  }
  // Joins the render thread before the SDL stream goes away.
  playback.reset();
  SDL_DestroyAudioStream(stream);
}

} // namespace aurora::uwp::audio
