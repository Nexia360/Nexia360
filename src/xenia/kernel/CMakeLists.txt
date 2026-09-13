add_library(xenia-kernel STATIC)
# Recursive, so a new .cc anywhere under kernel/ is picked up - but the glob
# runs at CONFIGURE time, not build time. Adding a file does not re-run CMake
# by itself; touching this file does.
xe_platform_sources(xenia-kernel ${CMAKE_CURRENT_SOURCE_DIR} RECURSIVE)
# touched to re-run the glob for xna/xna_direct_vulkan.cc
target_compile_definitions(xenia-kernel PRIVATE WITH_GZFILEOP)
target_include_directories(xenia-kernel PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
  ${PROJECT_SOURCE_DIR}/third_party/glslang
  ${PROJECT_SOURCE_DIR}/third_party/FFmpeg
  ${PROJECT_SOURCE_DIR}/third_party/ffmpeg-xenia
)
if(XE_TARGET_X86_64)
  target_compile_definitions(xenia-kernel PRIVATE
    X86_FEATURES
    X86_HAVE_XSAVE_INTRIN
    X86_SSE2
    X86_SSSE3
    X86_SSE42
    X86_AVX2
    X86_AVX512
    X86_AVX512VNNI
    X86_PCLMULQDQ_CRC
    X86_VPCLMULQDQ_CRC
    CURL_STATICLIB
    MINIUPNP_STATICLIB
  )
endif()
target_link_libraries(xenia-kernel PUBLIC
  aes_128 fmt sqlite zlib-ng pugixml libcurl miniupnp xenia-apu xenia-base xenia-cpu xenia-gpu xenia-hid xenia-vfs xenia-app-discord
  # A hosted XNA title never touches the guest GPU, so its frames are put on the
  # screen by talking to the presenter directly - see xna/xna_present.cc. That
  # needs the D3D12 presenter's declarations and the COM GUIDs IID_PPV_ARGS
  # expands to.
  xenia-ui-d3d12
  xenia-ui-vulkan
)
xe_target_defaults(xenia-kernel)

if(XENIA_BUILD_TESTS)
  set(CMAKE_FOLDER "tests")
  add_subdirectory(testing)
endif()
