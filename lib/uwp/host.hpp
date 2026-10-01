#pragma once

// UWP host: owns the CoreWindow and feeds SDL with window, keyboard and gamepad events.
//
// SDL3 has no UWP backend, so on UWP SDL runs with its offscreen video driver. The host
// creates the real CoreWindow, keeps the offscreen "proxy" SDL_Window's size in sync with
// it, and turns CoreWindow input and Windows.Gaming.Input gamepads into SDL events and
// virtual joysticks. The rest of aurora (and the app) keeps using SDL as usual.

#ifdef AURORA_UWP

#include <cstdint>

struct SDL_Window;

namespace aurora::uwp {

using MainFunction = int (*)(int argc, char* argv[]);

// Runs the UWP application, calling `main` on the UI thread once the CoreWindow exists.
int run(MainFunction main) noexcept;

// Size to render at, in physical pixels. On Xbox this is the TV's resolution, which can be larger than the
// CoreWindow.
void get_pixel_size(int& width, int& height) noexcept;

// Physical pixels per CoreWindow unit (2 when rendering at 3840x2160 on Xbox), used to scale the UI.
float get_content_scale() noexcept;

// Whether the app is running on an Xbox console.
bool is_xbox() noexcept;

// The CoreWindow as an IUnknown*, for creating a swapchain surface.
void* get_core_window() noexcept;

// Called once SDL's proxy window exists, and before it's destroyed.
void attach_window(SDL_Window* window) noexcept;
void detach_window() noexcept;

// Dispatches pending CoreWindow events and syncs gamepads. With `wait`, blocks until at
// least one CoreWindow event arrives (used while the app is paused in the background).
void pump_events(bool wait) noexcept;

} // namespace aurora::uwp

#endif
