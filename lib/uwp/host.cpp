#include "host.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <unknwn.h>
// C++/WinRT goes before aurora's headers, whose TRY macro collides with a WinRT method name.
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>

#include "../internal.hpp"
#include "../window.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace aurora::uwp {
namespace {
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Gaming::Input;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI::Core;

constexpr Module Log{"aurora::uwp"};

// Optional file in the package root and/or the local folder listing extra command line arguments, one per line.
// Lines starting with '#' are ignored. {InstalledLocation} and {LocalFolder} are replaced with
// the package install directory and the app's writable local data directory.
constexpr std::wstring_view LaunchArgumentsFile = L"aurora-args.txt";

MainFunction g_main = nullptr;
int g_exitCode = 0;

CoreWindow g_coreWindow{nullptr};
SDL_Window* g_proxyWindow = nullptr;
int g_pixelWidth = 0;
int g_pixelHeight = 0;
bool g_visible = true;

// Text input events point at strings that must outlive the event; keep the latest few alive.
std::deque<std::string> g_textStorage;
char16_t g_pendingHighSurrogate = 0;

std::string to_utf8(std::wstring_view str) { return winrt::to_string(str); }

void replace_all(std::string& str, std::string_view from, std::string_view to) {
  for (size_t pos = str.find(from); pos != std::string::npos; pos = str.find(from, pos + to.size())) {
    str.replace(pos, from.size(), to);
  }
}

std::vector<std::string> launch_arguments(const std::string& installedLocation, const std::string& localFolder) {
  std::vector<std::string> args;
  wchar_t modulePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
  args.push_back(to_utf8(modulePath));

  // Arguments from the package come first; a file in the local folder (which, unlike the package, can be edited on
  // the device) can add to them.
  for (const auto& dir : {installedLocation, localFolder}) {
    std::ifstream file{std::filesystem::path{winrt::to_hstring(dir).c_str()} / LaunchArgumentsFile};
    std::string line;
    while (std::getline(file, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
      }
      const auto start = line.find_first_not_of(" \t");
      if (start == std::string::npos || line[start] == '#') {
        continue;
      }
      line.erase(0, start);
      replace_all(line, "{InstalledLocation}", installedLocation);
      replace_all(line, "{LocalFolder}", localFolder);
      args.push_back(std::move(line));
    }
  }
  return args;
}

void update_window_size() {
  if (!g_coreWindow) {
    return;
  }
  const auto bounds = g_coreWindow.Bounds();
  const double scale = DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
  const int width = std::max(1, static_cast<int>(std::lround(bounds.Width * scale)));
  const int height = std::max(1, static_cast<int>(std::lround(bounds.Height * scale)));
  if (width == g_pixelWidth && height == g_pixelHeight) {
    return;
  }
  g_pixelWidth = width;
  g_pixelHeight = height;
  Log.info("CoreWindow size {}x{} ({}x{} at scale {})", width, height, bounds.Width, bounds.Height, scale);
  if (g_proxyWindow != nullptr) {
    // The offscreen driver turns this into the usual resize events.
    TRY_WARN(SDL_SetWindowSize(g_proxyWindow, width, height), "Failed to resize proxy window: {}", SDL_GetError());
  }
}

void set_visible(bool visible) {
  if (visible == g_visible) {
    return;
  }
  g_visible = visible;
  Log.info("App {}", visible ? "visible" : "hidden");
  window::set_backgrounded(!visible);
}

void push_event(SDL_Event& event) {
  if (!SDL_PushEvent(&event)) {
    Log.warn("Failed to push SDL event {}: {}", event.type, SDL_GetError());
  }
}

// Keyboard
SDL_Scancode to_scancode(VirtualKey key) {
  const auto code = static_cast<int>(key);
  if (code >= 'A' && code <= 'Z') {
    return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (code - 'A'));
  }
  if (code >= '1' && code <= '9') {
    return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (code - '1'));
  }
  if (code >= static_cast<int>(VirtualKey::F1) && code <= static_cast<int>(VirtualKey::F12)) {
    return static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (code - static_cast<int>(VirtualKey::F1)));
  }
  switch (key) {
  case VirtualKey::Number0:
    return SDL_SCANCODE_0;
  case VirtualKey::Enter:
    return SDL_SCANCODE_RETURN;
  case VirtualKey::Escape:
    return SDL_SCANCODE_ESCAPE;
  case VirtualKey::Back:
    return SDL_SCANCODE_BACKSPACE;
  case VirtualKey::Tab:
    return SDL_SCANCODE_TAB;
  case VirtualKey::Space:
    return SDL_SCANCODE_SPACE;
  case VirtualKey::Left:
    return SDL_SCANCODE_LEFT;
  case VirtualKey::Right:
    return SDL_SCANCODE_RIGHT;
  case VirtualKey::Up:
    return SDL_SCANCODE_UP;
  case VirtualKey::Down:
    return SDL_SCANCODE_DOWN;
  case VirtualKey::Insert:
    return SDL_SCANCODE_INSERT;
  case VirtualKey::Delete:
    return SDL_SCANCODE_DELETE;
  case VirtualKey::Home:
    return SDL_SCANCODE_HOME;
  case VirtualKey::End:
    return SDL_SCANCODE_END;
  case VirtualKey::PageUp:
    return SDL_SCANCODE_PAGEUP;
  case VirtualKey::PageDown:
    return SDL_SCANCODE_PAGEDOWN;
  case VirtualKey::LeftShift:
  case VirtualKey::Shift:
    return SDL_SCANCODE_LSHIFT;
  case VirtualKey::RightShift:
    return SDL_SCANCODE_RSHIFT;
  case VirtualKey::LeftControl:
  case VirtualKey::Control:
    return SDL_SCANCODE_LCTRL;
  case VirtualKey::RightControl:
    return SDL_SCANCODE_RCTRL;
  case VirtualKey::LeftMenu:
  case VirtualKey::Menu:
    return SDL_SCANCODE_LALT;
  case VirtualKey::RightMenu:
    return SDL_SCANCODE_RALT;
  default:
    break;
  }
  // Punctuation keys only exist as OEM virtual key codes.
  switch (code) {
  case VK_OEM_1:
    return SDL_SCANCODE_SEMICOLON;
  case VK_OEM_PLUS:
    return SDL_SCANCODE_EQUALS;
  case VK_OEM_COMMA:
    return SDL_SCANCODE_COMMA;
  case VK_OEM_MINUS:
    return SDL_SCANCODE_MINUS;
  case VK_OEM_PERIOD:
    return SDL_SCANCODE_PERIOD;
  case VK_OEM_2:
    return SDL_SCANCODE_SLASH;
  case VK_OEM_3:
    return SDL_SCANCODE_GRAVE;
  case VK_OEM_4:
    return SDL_SCANCODE_LEFTBRACKET;
  case VK_OEM_5:
    return SDL_SCANCODE_BACKSLASH;
  case VK_OEM_6:
    return SDL_SCANCODE_RIGHTBRACKET;
  case VK_OEM_7:
    return SDL_SCANCODE_APOSTROPHE;
  default:
    // Includes the gamepad and navigation virtual keys Xbox sends alongside gamepad input,
    // which are handled through Windows.Gaming.Input instead.
    return SDL_SCANCODE_UNKNOWN;
  }
}

SDL_Keymod current_modifiers() {
  const auto down = [](VirtualKey key) {
    return (g_coreWindow.GetKeyState(key) & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
  };
  SDL_Keymod mod = SDL_KMOD_NONE;
  if (down(VirtualKey::LeftShift)) mod |= SDL_KMOD_LSHIFT;
  if (down(VirtualKey::RightShift)) mod |= SDL_KMOD_RSHIFT;
  if (down(VirtualKey::LeftControl)) mod |= SDL_KMOD_LCTRL;
  if (down(VirtualKey::RightControl)) mod |= SDL_KMOD_RCTRL;
  if (down(VirtualKey::LeftMenu)) mod |= SDL_KMOD_LALT;
  if (down(VirtualKey::RightMenu)) mod |= SDL_KMOD_RALT;
  return mod;
}

void on_key(const KeyEventArgs& args, bool down) {
  const SDL_Scancode scancode = to_scancode(args.VirtualKey());
  if (scancode == SDL_SCANCODE_UNKNOWN || g_proxyWindow == nullptr) {
    return;
  }
  args.Handled(true);
  const SDL_Keymod mod = current_modifiers();
  SDL_Event event{};
  event.key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.timestamp = SDL_GetTicksNS();
  event.key.windowID = SDL_GetWindowID(g_proxyWindow);
  event.key.scancode = scancode;
  event.key.key = SDL_GetKeyFromScancode(scancode, mod, true);
  event.key.mod = mod;
  event.key.raw = static_cast<Uint16>(args.VirtualKey());
  event.key.down = down;
  event.key.repeat = down && args.KeyStatus().WasKeyDown;
  push_event(event);
}

void on_character(const CharacterReceivedEventArgs& args) {
  if (g_proxyWindow == nullptr || !SDL_TextInputActive(g_proxyWindow)) {
    return;
  }
  const auto unit = static_cast<char16_t>(args.KeyCode());
  std::u16string text;
  if (unit >= 0xD800 && unit <= 0xDBFF) {
    g_pendingHighSurrogate = unit;
    return;
  }
  if (unit >= 0xDC00 && unit <= 0xDFFF) {
    if (g_pendingHighSurrogate == 0) {
      return;
    }
    text = {g_pendingHighSurrogate, unit};
    g_pendingHighSurrogate = 0;
  } else if (unit < 0x20 || unit == 0x7F) {
    // Control characters arrive as key events
    return;
  } else {
    text = {unit};
  }
  args.Handled(true);
  g_textStorage.push_back(to_utf8({reinterpret_cast<const wchar_t*>(text.data()), text.size()}));
  while (g_textStorage.size() > 64) {
    g_textStorage.pop_front();
  }
  SDL_Event event{};
  event.text.type = SDL_EVENT_TEXT_INPUT;
  event.text.timestamp = SDL_GetTicksNS();
  event.text.windowID = SDL_GetWindowID(g_proxyWindow);
  event.text.text = g_textStorage.back().c_str();
  push_event(event);
}

// Gamepads
//
// Windows.Gaming.Input gamepads are exposed to SDL as virtual joysticks of type gamepad, so the
// app sees them as regular SDL gamepads. SDL's mapping for a virtual gamepad assigns joystick
// buttons and axes in SDL_GamepadButton/SDL_GamepadAxis order, skipping ones not in the masks.
struct ButtonMapping {
  SDL_GamepadButton sdl;
  GamepadButtons wgi;
};
constexpr ButtonMapping ButtonMappings[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, GamepadButtons::A},
    {SDL_GAMEPAD_BUTTON_EAST, GamepadButtons::B},
    {SDL_GAMEPAD_BUTTON_WEST, GamepadButtons::X},
    {SDL_GAMEPAD_BUTTON_NORTH, GamepadButtons::Y},
    {SDL_GAMEPAD_BUTTON_BACK, GamepadButtons::View},
    {SDL_GAMEPAD_BUTTON_START, GamepadButtons::Menu},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, GamepadButtons::LeftThumbstick},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, GamepadButtons::RightThumbstick},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, GamepadButtons::LeftShoulder},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, GamepadButtons::RightShoulder},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, GamepadButtons::DPadUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, GamepadButtons::DPadDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, GamepadButtons::DPadLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, GamepadButtons::DPadRight},
};

struct VirtualGamepad {
  Gamepad gamepad{nullptr};
  SDL_JoystickID id = 0;
  SDL_Joystick* joystick = nullptr;
  GamepadVibration vibration{};
};
std::vector<std::unique_ptr<VirtualGamepad>> g_gamepads;
std::atomic_bool g_gamepadsChanged = true;
winrt::event_token g_gamepadAddedToken;
winrt::event_token g_gamepadRemovedToken;

Sint16 to_axis(double value) {
  return static_cast<Sint16>(std::clamp(std::lround(value * SDL_JOYSTICK_AXIS_MAX), long{SDL_JOYSTICK_AXIS_MIN},
                                        long{SDL_JOYSTICK_AXIS_MAX}));
}

void SDLCALL update_gamepad(void* userdata) {
  auto* pad = static_cast<VirtualGamepad*>(userdata);
  if (pad->joystick == nullptr) {
    return;
  }
  GamepadReading reading;
  try {
    reading = pad->gamepad.GetCurrentReading();
  } catch (const winrt::hresult_error&) {
    return;
  }
  SDL_SetJoystickVirtualAxis(pad->joystick, SDL_GAMEPAD_AXIS_LEFTX, to_axis(reading.LeftThumbstickX));
  SDL_SetJoystickVirtualAxis(pad->joystick, SDL_GAMEPAD_AXIS_LEFTY, to_axis(-reading.LeftThumbstickY));
  SDL_SetJoystickVirtualAxis(pad->joystick, SDL_GAMEPAD_AXIS_RIGHTX, to_axis(reading.RightThumbstickX));
  SDL_SetJoystickVirtualAxis(pad->joystick, SDL_GAMEPAD_AXIS_RIGHTY, to_axis(-reading.RightThumbstickY));
  SDL_SetJoystickVirtualAxis(pad->joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, to_axis(reading.LeftTrigger));
  SDL_SetJoystickVirtualAxis(pad->joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, to_axis(reading.RightTrigger));
  int index = 0;
  for (const auto& mapping : ButtonMappings) {
    SDL_SetJoystickVirtualButton(pad->joystick, index++, (reading.Buttons & mapping.wgi) == mapping.wgi);
  }
}

void apply_vibration(VirtualGamepad& pad) {
  try {
    pad.gamepad.Vibration(pad.vibration);
  } catch (const winrt::hresult_error&) {
  }
}

bool SDLCALL rumble_gamepad(void* userdata, Uint16 low, Uint16 high) {
  auto* pad = static_cast<VirtualGamepad*>(userdata);
  pad->vibration.LeftMotor = low / 65535.0;
  pad->vibration.RightMotor = high / 65535.0;
  apply_vibration(*pad);
  return true;
}

bool SDLCALL rumble_gamepad_triggers(void* userdata, Uint16 left, Uint16 right) {
  auto* pad = static_cast<VirtualGamepad*>(userdata);
  pad->vibration.LeftTrigger = left / 65535.0;
  pad->vibration.RightTrigger = right / 65535.0;
  apply_vibration(*pad);
  return true;
}

void attach_gamepad(const Gamepad& gamepad) {
  auto pad = std::make_unique<VirtualGamepad>();
  pad->gamepad = gamepad;

  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  // Report an Xbox One controller so the app shows Xbox button labels
  desc.vendor_id = 0x045E;
  desc.product_id = 0x02EA;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
  desc.nbuttons = static_cast<Uint16>(std::size(ButtonMappings));
  for (const auto& mapping : ButtonMappings) {
    desc.button_mask |= 1u << mapping.sdl;
  }
  desc.name = "Xbox Controller";
  desc.userdata = pad.get();
  desc.Update = update_gamepad;
  desc.Rumble = rumble_gamepad;
  desc.RumbleTriggers = rumble_gamepad_triggers;

  pad->id = SDL_AttachVirtualJoystick(&desc);
  if (pad->id == 0) {
    Log.error("Failed to attach virtual gamepad: {}", SDL_GetError());
    return;
  }
  pad->joystick = SDL_OpenJoystick(pad->id);
  if (pad->joystick == nullptr) {
    Log.error("Failed to open virtual gamepad: {}", SDL_GetError());
    SDL_DetachVirtualJoystick(pad->id);
    return;
  }
  Log.info("Gamepad connected (joystick {})", pad->id);
  g_gamepads.push_back(std::move(pad));
}

void detach_gamepad(VirtualGamepad& pad) {
  Log.info("Gamepad disconnected (joystick {})", pad.id);
  pad.vibration = {};
  apply_vibration(pad);
  SDL_CloseJoystick(pad.joystick);
  SDL_DetachVirtualJoystick(pad.id);
}

void detach_all_gamepads() {
  if (SDL_WasInit(SDL_INIT_JOYSTICK) != 0) {
    for (const auto& pad : g_gamepads) {
      detach_gamepad(*pad);
    }
  }
  g_gamepads.clear();
  g_gamepadsChanged = true;
}

void sync_gamepads() {
  // Virtual joysticks can only be attached once the app has initialized SDL's joystick subsystem.
  if (SDL_WasInit(SDL_INIT_JOYSTICK) == 0 || !g_gamepadsChanged.exchange(false)) {
    return;
  }
  std::vector<Gamepad> connected;
  try {
    for (const auto& gamepad : Gamepad::Gamepads()) {
      connected.push_back(gamepad);
    }
  } catch (const winrt::hresult_error& e) {
    Log.warn("Failed to list gamepads: {}", to_utf8(e.message()));
    g_gamepadsChanged = true;
    return;
  }
  std::erase_if(g_gamepads, [&](const std::unique_ptr<VirtualGamepad>& pad) {
    if (std::ranges::find(connected, pad->gamepad) != connected.end()) {
      return false;
    }
    detach_gamepad(*pad);
    return true;
  });
  for (const auto& gamepad : connected) {
    if (std::ranges::none_of(g_gamepads, [&](const auto& pad) { return pad->gamepad == gamepad; })) {
      attach_gamepad(gamepad);
    }
  }
}

struct App : winrt::implements<App, IFrameworkViewSource, IFrameworkView> {
  IFrameworkView CreateView() { return *this; }

  void Initialize(const CoreApplicationView& view) {
    view.Activated([](const auto&, const IActivatedEventArgs&) { CoreWindow::GetForCurrentThread().Activate(); });
    CoreApplication::Suspending([](const auto&, const SuspendingEventArgs&) { Log.info("Suspending"); });
    CoreApplication::Resuming([](const auto&, const auto&) { Log.info("Resuming"); });
  }

  void SetWindow(const CoreWindow& window) {
    g_coreWindow = window;
    window.SizeChanged([](const auto&, const WindowSizeChangedEventArgs&) { update_window_size(); });
    window.VisibilityChanged([](const auto&, const VisibilityChangedEventArgs& args) { set_visible(args.Visible()); });
    window.Closed([](const auto&, const CoreWindowEventArgs&) {
      SDL_Event event{.type = SDL_EVENT_QUIT};
      event.quit.timestamp = SDL_GetTicksNS();
      push_event(event);
    });
    window.KeyDown([](const auto&, const KeyEventArgs& args) { on_key(args, true); });
    window.KeyUp([](const auto&, const KeyEventArgs& args) { on_key(args, false); });
    window.CharacterReceived([](const auto&, const CharacterReceivedEventArgs& args) { on_character(args); });
    DisplayInformation::GetForCurrentView().DpiChanged([](const auto&, const auto&) { update_window_size(); });
    // On Xbox, B raises a back request; unhandled, it would send the app to the background.
    SystemNavigationManager::GetForCurrentView().BackRequested(
        [](const auto&, const BackRequestedEventArgs& args) { args.Handled(true); });
    g_gamepadAddedToken = Gamepad::GamepadAdded([](const auto&, const auto&) { g_gamepadsChanged = true; });
    g_gamepadRemovedToken = Gamepad::GamepadRemoved([](const auto&, const auto&) { g_gamepadsChanged = true; });
  }

  void Load(const winrt::hstring&) {}

  void Run() {
    g_coreWindow.Activate();
    g_coreWindow.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
    update_window_size();

    const auto installedLocation = to_utf8(Package::Current().InstalledLocation().Path());
    const auto localFolder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
    // Read by SDL_GetPrefPath (see apply-sdl3-uwp-host.cmake)
    SetEnvironmentVariableW(L"SDL_UWP_PREF_ROOT", localFolder.c_str());

    auto args = launch_arguments(installedLocation, to_utf8(localFolder));
    std::vector<char*> argv;
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    g_exitCode = g_main(static_cast<int>(args.size()), argv.data());
  }

  void Uninitialize() {
    Gamepad::GamepadAdded(g_gamepadAddedToken);
    Gamepad::GamepadRemoved(g_gamepadRemovedToken);
    g_coreWindow = nullptr;
  }
};
} // namespace

int run(MainFunction main) noexcept {
  g_main = main;
  try {
    winrt::init_apartment();
    CoreApplication::Run(winrt::make<App>());
  } catch (const winrt::hresult_error& e) {
    Log.fatal("Unhandled WinRT error: {}", to_utf8(e.message()));
  }
  return g_exitCode;
}

void get_pixel_size(int& width, int& height) noexcept {
  width = g_pixelWidth;
  height = g_pixelHeight;
}

bool is_xbox() noexcept {
  static const bool xbox = [] {
    try {
      return winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox";
    } catch (const winrt::hresult_error&) {
      return false;
    }
  }();
  return xbox;
}

void* get_core_window() noexcept { return g_coreWindow ? winrt::get_unknown(g_coreWindow) : nullptr; }

void attach_window(SDL_Window* window) noexcept {
  g_proxyWindow = window;
  if (window != nullptr && g_pixelWidth > 0 && g_pixelHeight > 0) {
    TRY_WARN(SDL_SetWindowSize(window, g_pixelWidth, g_pixelHeight), "Failed to resize proxy window: {}",
             SDL_GetError());
  }
}

void detach_window() noexcept {
  detach_all_gamepads();
  g_proxyWindow = nullptr;
}

void pump_events(bool wait) noexcept {
  if (g_coreWindow) {
    try {
      g_coreWindow.Dispatcher().ProcessEvents(wait ? CoreProcessEventsOption::ProcessOneAndAllPending
                                                   : CoreProcessEventsOption::ProcessAllIfPresent);
    } catch (const winrt::hresult_error& e) {
      Log.error("Failed to process CoreWindow events: {}", to_utf8(e.message()));
    }
  }
  sync_gamepads();
}

} // namespace aurora::uwp
