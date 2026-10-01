#include <aurora/main.h>
#undef main

#include "host.hpp"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <Windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // There's no SDL_main entry point on UWP; aurora's host calls the app's main instead.
  SDL_SetMainReady();
  return aurora::uwp::run(aurora_main);
}
