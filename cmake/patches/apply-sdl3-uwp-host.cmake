# Lets SDL3 build for a UWP app whose window, input and audio come from aurora's UWP host.
#
# SDL3 dropped WinRT support, and its Win32 video and joystick drivers depend on desktop-only
# APIs (window classes, raw input, SetupAPI) that are not available to UWP apps on Xbox.
# When SDL_UWP_HOST is set, those drivers are left out; SDL then falls back to its offscreen
# video driver and virtual joysticks, which aurora's UWP host drives.
if (NOT DEFINED SDL_SOURCE_DIR)
  message(FATAL_ERROR "SDL_SOURCE_DIR is required")
endif ()

# Replaces one known snippet in a file's contents, failing if it's neither there nor already replaced.
function(_sdl_uwp_replace source_var old new)
  string(FIND "${${source_var}}" "${new}" _already_patched)
  if (NOT _already_patched EQUAL -1)
    return()
  endif ()
  string(FIND "${${source_var}}" "${old}" _patch_site)
  if (_patch_site EQUAL -1)
    message(FATAL_ERROR "Failed to apply SDL3 UWP host patch: expected text not found:\n${old}")
  endif ()
  string(REPLACE "${old}" "${new}" _text "${${source_var}}")
  set(${source_var} "${_text}" PARENT_SCOPE)
endfunction()

set(_cmakelists "${SDL_SOURCE_DIR}/CMakeLists.txt")
set(_filesystem "${SDL_SOURCE_DIR}/src/filesystem/windows/SDL_sysfilesystem.c")
set(_sdl "${SDL_SOURCE_DIR}/src/SDL.c")
file(READ "${_cmakelists}" _cmake_source)
file(READ "${_filesystem}" _filesystem_source)
file(READ "${_sdl}" _sdl_source)
foreach (_var _cmake_source _filesystem_source _sdl_source)
  string(REPLACE "
" "
" ${_var} "${${_var}}")
endforeach ()

_sdl_uwp_replace(_cmake_source
  "  if(SDL_VIDEO)\n    set(SDL_VIDEO_DRIVER_WINDOWS 1)"
  "  if(SDL_VIDEO AND NOT SDL_UWP_HOST)\n    set(SDL_VIDEO_DRIVER_WINDOWS 1)")
_sdl_uwp_replace(_cmake_source
  "  if(SDL_JOYSTICK)\n    sdl_glob_sources(\n      \"\${SDL3_SOURCE_DIR}/src/joystick/windows/*.c\""
  "  if(SDL_JOYSTICK AND NOT SDL_UWP_HOST)\n    sdl_glob_sources(\n      \"\${SDL3_SOURCE_DIR}/src/joystick/windows/*.c\"")

# Packaged apps can't write to the user's roaming AppData, so SDL_GetPrefPath uses the
# app's LocalFolder, which the host publishes in SDL_UWP_PREF_ROOT before SDL starts.
_sdl_uwp_replace(_filesystem_source
  "    hr = SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, path);\n"
  "#ifdef SDL_UWP_HOST\n    {\n        const DWORD len = GetEnvironmentVariableW(L\"SDL_UWP_PREF_ROOT\", path, MAX_PATH);\n        hr = (len > 0 && len < MAX_PATH) ? S_OK : E_FAIL;\n    }\n#else\n    hr = SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, path);\n#endif\n")

# The hidden helper window only serves the DirectInput drivers, which are left out, and Xbox
# doesn't allow registering window classes.
_sdl_uwp_replace(_sdl_source
  "#ifdef SDL_PLATFORM_WINDOWS\n    if (flags & (SDL_INIT_HAPTIC | SDL_INIT_JOYSTICK)) {\n        if (!SDL_HelperWindowCreate()) {"
  "#if defined(SDL_PLATFORM_WINDOWS) && !defined(SDL_UWP_HOST)\n    if (flags & (SDL_INIT_HAPTIC | SDL_INIT_JOYSTICK)) {\n        if (!SDL_HelperWindowCreate()) {")

file(WRITE "${_cmakelists}" "${_cmake_source}")
file(WRITE "${_filesystem}" "${_filesystem_source}")
file(WRITE "${_sdl}" "${_sdl_source}")
message(STATUS "Applied SDL3 UWP host patch")
