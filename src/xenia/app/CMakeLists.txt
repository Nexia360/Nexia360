# src/xenia/app/CMakeLists.txt - Main xenia_canary executable.

if(WIN32)
  add_executable(xenia-app WIN32)
else()
  add_executable(xenia-app)
endif()

set_target_properties(xenia-app PROPERTIES OUTPUT_NAME "Nexia360")

# Main source
target_sources(xenia-app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/xenia_main.cc
)

# Other app sources (emulator_window, profile_dialogs, etc.)
# touched to re-run the glob for avatar_editor_dialog.cc and xui_scene_dialog.cc,
# plus recent_titles_dialog.cc added 2026-09-19
xe_platform_sources(xenia-app ${CMAKE_CURRENT_SOURCE_DIR})

# Platform-specific entry points
if(WIN32)
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_win.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_win.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/main_resources.rc
    ${PROJECT_SOURCE_DIR}/src/xenia/base/app_win32.manifest
  )
  get_property(XE_XNA_PAYLOAD_RC GLOBAL PROPERTY XE_XNA_PAYLOAD_RC)
  if(XE_XNA_PAYLOAD_RC)
    set_source_files_properties(${XE_XNA_PAYLOAD_RC} PROPERTIES GENERATED TRUE)
    target_sources(xenia-app PRIVATE ${XE_XNA_PAYLOAD_RC})
  endif()
  if(XE_TARGET_X86_64)
    # main_init_win.cc needs SSE2 only (not AVX)
    set_source_files_properties(
      ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_win.cc
      PROPERTIES COMPILE_OPTIONS "/arch:SSE2"
    )
  endif()
else()
  target_sources(xenia-app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_posix.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/ui/windowed_app_main_posix.cc
  )
  if(NOT MSVC AND XE_TARGET_X86_64)
    set_source_files_properties(
      ${PROJECT_SOURCE_DIR}/src/xenia/base/main_init_posix.cc
      PROPERTIES COMPILE_OPTIONS "-msse2;-mno-avx"
    )
  endif()
endif()

target_compile_definitions(xenia-app PRIVATE
  XBYAK_NO_OP_NAMES
  XBYAK_ENABLE_OMITTED_OPERAND
  CURL_STATICLIB
)

# Core libraries (all platforms)
target_link_libraries(xenia-app PRIVATE
  xenia-apu
  xenia-apu-nop
  xenia-base
  xenia-core
  xenia-cpu
  xenia-gpu
  xenia-gpu-null
  xenia-hid
  xenia-hid-nop
  xenia-kernel
  xenia-patcher
  xenia-ui
  xenia-vfs
  # Third-party
  aes_128
  capstone
  fmt
  dxbc
  discord-rpc
  imgui
  libavcodec
  libavutil
  mspack
  snappy
  xxhash
  libcurl
)

# Vulkan/SPIR-V backends (not macOS)
if(NOT APPLE)
  target_link_libraries(xenia-app PRIVATE
    xenia-gpu-vulkan
    xenia-ui-vulkan
    glslang-spirv
  )
endif()

# Non-Android specific (always true for Windows/Linux)
target_link_libraries(xenia-app PRIVATE
  xenia-app-discord
  xenia-apu-sdl
  xenia-debug-ui
  xenia-helper-sdl
  xenia-hid-sdl
)

# Architecture-specific backend
if(XE_TARGET_X86_64)
  target_link_libraries(xenia-app PRIVATE xenia-cpu-backend-x64)
elseif(XE_TARGET_AARCH64)
  target_link_libraries(xenia-app PRIVATE xenia-cpu-backend-a64)
endif()

# Platform-specific libraries
if(WIN32)
  target_link_libraries(xenia-app PRIVATE
    xenia-apu-xaudio2
    xenia-gpu-d3d12
    xenia-hid-winkey
    xenia-hid-xinput
    xenia-ui-d3d12
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(xenia-app PRIVATE
    xenia-apu-alsa
    X11
    xcb
    X11-xcb
    SDL2
  )
endif()

xe_target_defaults(xenia-app)

# The managed side of the XNA host (Nexia.Xna.Host.dll) is a .NET assembly, not
# part of this build. It is published beside the emulator so hosted XNA titles
# can find it - hostfxr needs the .runtimeconfig.json next to the dll, so both
# travel together.
#
# Guarded on dotnet being present: a machine without the SDK still builds the
# emulator, it just cannot run XNA titles - which is the same thing that
# happens without the .NET runtime at all.
# The console's XNA assemblies P/Invoke into modules that only exist on an
# Xbox. Exporting those entry points from the executable lets the managed side
# bind them here directly, with no IL rewriting. The names are C++-shaped and
# not legal C identifiers, so a .def file maps them onto the C symbols.
if(WIN32)
  target_link_options(xenia-app PRIVATE
    "/DEF:${PROJECT_SOURCE_DIR}/src/xenia/kernel/xna/xna_exports.def")
  set_property(TARGET xenia-app APPEND PROPERTY LINK_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/xenia/kernel/xna/xna_exports.def)
endif()
