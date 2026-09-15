add_library(xenia-kernel STATIC)
# Recursive, so a new .cc anywhere under kernel/ is picked up - but the glob
# runs at CONFIGURE time, not build time. Adding a file does not re-run CMake
# by itself; touching this file does.
xe_platform_sources(xenia-kernel ${CMAKE_CURRENT_SOURCE_DIR} RECURSIVE)
# touched to re-run the glob for xam/xam_avatar_assets.cc

set(XE_XNA_MANAGED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/xna/managed)
set(XE_XNA_HOST_CSPROJ ${XE_XNA_MANAGED_DIR}/Nexia.Xna.Host.csproj)
set(XE_XNA_PUBLISH_DIR ${CMAKE_CURRENT_BINARY_DIR}/xna_host_publish)
set(XE_XNA_PAYLOAD_CC ${CMAKE_CURRENT_BINARY_DIR}/xna_host_payload.cc)
set(XE_XNA_PAYLOAD_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/xna/embed_host_payload.cmake)
set(XE_XNA_HOST_VERSION "unversioned")
if(EXISTS ${XE_XNA_HOST_CSPROJ})
  file(READ ${XE_XNA_HOST_CSPROJ} XE_XNA_CSPROJ_TEXT)
  string(REGEX MATCH "<Version>([^<]*)</Version>" _ "${XE_XNA_CSPROJ_TEXT}")
  set(XE_XNA_HOST_BASE_VERSION "${CMAKE_MATCH_1}")
  string(REGEX MATCH "<NexiaShimRevision>([^<]*)</NexiaShimRevision>" _
         "${XE_XNA_CSPROJ_TEXT}")
  set(XE_XNA_HOST_REVISION "${CMAKE_MATCH_1}")
  if(XE_XNA_HOST_BASE_VERSION)
    set(XE_XNA_HOST_VERSION
        "${XE_XNA_HOST_BASE_VERSION}+shim.${XE_XNA_HOST_REVISION}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${XE_XNA_HOST_CSPROJ})
endif()

if(WIN32)
  set(XE_XNA_PAYLOAD_MODE resource)
  if(XE_TARGET_AARCH64)
    set(XE_XNA_HOST_RID win-arm64)
  else()
    set(XE_XNA_HOST_RID win-x64)
  endif()
  set(XE_XNA_NATIVE_NAMES SDL2.dll openal.dll)
else()
  set(XE_XNA_PAYLOAD_MODE array)
  if(XE_TARGET_AARCH64)
    set(XE_XNA_HOST_RID linux-arm64)
  else()
    set(XE_XNA_HOST_RID linux-x64)
  endif()
  set(XE_XNA_NATIVE_NAMES libSDL2-2.0.so.0 libopenal.so)
endif()
set(XE_XNA_PAYLOAD_RC ${CMAKE_CURRENT_BINARY_DIR}/xna_host_payload.rc)
set_property(GLOBAL PROPERTY XE_XNA_PAYLOAD_RC ${XE_XNA_PAYLOAD_RC})

set(XE_XNA_CONSOLE_RUNTIME_DIR "" CACHE PATH
    "Folder holding the console XNA runtime (MXF*.dlx and the Compact Framework .dlx files) to embed in Nexia; empty uses xna/console_runtime")
set(XE_XNA_CONSOLE_DIR "${XE_XNA_CONSOLE_RUNTIME_DIR}")
if(NOT XE_XNA_CONSOLE_DIR)
  set(XE_XNA_CONSOLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/xna/console_runtime)
endif()

set(XE_XNA_PAYLOAD_ENTRIES
    Nexia.Xna.Host.dll
    Nexia.Xna.Host.runtimeconfig.json
    Nexia.Xna.Host.deps.json
    Mono.Cecil.dll)
foreach(native IN LISTS XE_XNA_NATIVE_NAMES)
  list(APPEND XE_XNA_PAYLOAD_ENTRIES runtimes/${XE_XNA_HOST_RID}/native/${native})
endforeach()
set(XE_XNA_CONSOLE_FILES "")
if(EXISTS "${XE_XNA_CONSOLE_DIR}")
  file(TO_CMAKE_PATH "${XE_XNA_CONSOLE_DIR}" XE_XNA_CONSOLE_DIR_NORMALIZED)
  file(GLOB XE_XNA_CONSOLE_FILES CONFIGURE_DEPENDS
       ${XE_XNA_CONSOLE_DIR_NORMALIZED}/*.dlx)
  if(XE_XNA_CONSOLE_FILES)
    message(STATUS
      "Embedding the console XNA runtime from ${XE_XNA_CONSOLE_DIR_NORMALIZED}")
  else()
    message(WARNING
      "XE_XNA_CONSOLE_RUNTIME_DIR=${XE_XNA_CONSOLE_DIR_NORMALIZED} holds no .dlx files")
  endif()
  foreach(console_file IN LISTS XE_XNA_CONSOLE_FILES)
    get_filename_component(console_name ${console_file} NAME)
    list(APPEND XE_XNA_PAYLOAD_ENTRIES console/${console_name}=${console_file})
  endforeach()
else()
  message(WARNING
    "No console XNA runtime at ${XE_XNA_CONSOLE_DIR} - XNA titles will not launch from this build")
endif()
string(JOIN "," XE_XNA_PAYLOAD_FILES ${XE_XNA_PAYLOAD_ENTRIES})

find_program(XE_DOTNET_EXECUTABLE dotnet)
if(XE_DOTNET_EXECUTABLE AND EXISTS ${XE_XNA_HOST_CSPROJ})
  file(GLOB_RECURSE XE_XNA_MANAGED_SOURCES CONFIGURE_DEPENDS
       ${XE_XNA_MANAGED_DIR}/*.cs ${XE_XNA_MANAGED_DIR}/*.skip)
  list(FILTER XE_XNA_MANAGED_SOURCES EXCLUDE REGEX "/(bin|obj)/")
  add_custom_command(
    OUTPUT ${XE_XNA_PAYLOAD_CC} ${XE_XNA_PAYLOAD_RC}
    COMMAND ${XE_DOTNET_EXECUTABLE} publish ${XE_XNA_HOST_CSPROJ}
            -c Release --nologo -v quiet -o ${XE_XNA_PUBLISH_DIR}
    COMMAND ${CMAKE_COMMAND}
            -DINPUT_DIR=${XE_XNA_PUBLISH_DIR}
            "-DFILES=${XE_XNA_PAYLOAD_FILES}"
            -DVERSION=${XE_XNA_HOST_VERSION}
            -DMODE=${XE_XNA_PAYLOAD_MODE}
            -DOUTPUT=${XE_XNA_PAYLOAD_CC}
            -DRC_OUTPUT=${XE_XNA_PAYLOAD_RC}
            -P ${XE_XNA_PAYLOAD_SCRIPT}
    DEPENDS ${XE_XNA_MANAGED_SOURCES} ${XE_XNA_HOST_CSPROJ}
            ${XE_XNA_MANAGED_DIR}/lib/Mono.Cecil.dll ${XE_XNA_PAYLOAD_SCRIPT}
            ${XE_XNA_CONSOLE_FILES}
    COMMENT "Publishing Nexia.Xna.Host ${XE_XNA_HOST_VERSION} and embedding it"
    VERBATIM
  )
else()
  message(STATUS
    "dotnet not found - XNA titles will not be launchable from this build")
  add_custom_command(
    OUTPUT ${XE_XNA_PAYLOAD_CC} ${XE_XNA_PAYLOAD_RC}
    COMMAND ${CMAKE_COMMAND}
            -DVERSION=${XE_XNA_HOST_VERSION}
            -DMODE=${XE_XNA_PAYLOAD_MODE}
            -DOUTPUT=${XE_XNA_PAYLOAD_CC}
            -DRC_OUTPUT=${XE_XNA_PAYLOAD_RC}
            -P ${XE_XNA_PAYLOAD_SCRIPT}
    DEPENDS ${XE_XNA_PAYLOAD_SCRIPT}
    VERBATIM
  )
endif()
target_sources(xenia-kernel PRIVATE ${XE_XNA_PAYLOAD_CC})

set(XE_XNA_INSTALL_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/xna/installer/install.ps1)
set(XE_XNA_INSTALL_SCRIPT_CC ${CMAKE_CURRENT_BINARY_DIR}/xna_install_script.cc)
set(XE_XNA_EMBED_FILE_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/xna/embed_file.cmake)
add_custom_command(
  OUTPUT ${XE_XNA_INSTALL_SCRIPT_CC}
  COMMAND ${CMAKE_COMMAND}
          -DINPUT=${XE_XNA_INSTALL_SCRIPT}
          -DOUTPUT=${XE_XNA_INSTALL_SCRIPT_CC}
          -DSYMBOL=kXnaInstallScript
          -DHEADER=xenia/kernel/xna/xna_runtime_install.h
          -P ${XE_XNA_EMBED_FILE_SCRIPT}
  DEPENDS ${XE_XNA_INSTALL_SCRIPT} ${XE_XNA_EMBED_FILE_SCRIPT}
  COMMENT "Embedding the XNA runtime installer"
  VERBATIM
)
target_sources(xenia-kernel PRIVATE ${XE_XNA_INSTALL_SCRIPT_CC})

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
