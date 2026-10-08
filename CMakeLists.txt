cmake_minimum_required(VERSION 3.24)
project(SmartWallpaper LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
  message(FATAL_ERROR "MSVC is required (C++20 + Win32 + D3D11)")
endif()

add_executable(WallpaperRenderer WIN32
  main.cpp
  resources/app.rc
  Engine/Core/Log.cpp
  Engine/Core/Json.cpp
  Engine/Graphics/D3D11Device.cpp
  Engine/Graphics/Shader.cpp
  Engine/Graphics/Texture.cpp
  Engine/Graphics/ParticleSystem.cpp
  Engine/Graphics/D3D11Renderer.cpp
  Engine/Graphics/Nv12Uploader.cpp
  Engine/Graphics/FFmpegPlayer.cpp
  Engine/Packages/WallpaperPackage.cpp
  Engine/Packages/WallpaperInstance.cpp
  Engine/Packages/WallpaperManager.cpp
  Engine/Packages/Thumbnailer.cpp
  Engine/Desktop/MonitorManager.cpp
  Engine/Desktop/WorkerW.cpp
  Engine/Desktop/TrayTransparency.cpp
  Engine/Desktop/DesktopWindow.cpp
  Engine/Performance/FPSController.cpp
  Engine/Performance/PowerManager.cpp
  Engine/App/Application.cpp
  Engine/App/Tray.cpp
  Engine/App/IPCServer.cpp
  Engine/App/IPCClient.cpp
)

target_include_directories(WallpaperRenderer PRIVATE "${CMAKE_SOURCE_DIR}")

target_compile_definitions(WallpaperRenderer PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00
)

target_compile_options(WallpaperRenderer PRIVATE
  /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc /MP /Gw
  $<$<CONFIG:Release>:/O2 /Oi>
)

# d3dcompiler_47.dll is loaded at runtime (it is in System32), so it is deliberately not linked.
# The mf* libraries are gone with the Media Foundation decoder (2026-10-07): the only Media Foundation
# left in this repo is in tools/, which builds its own executables.
target_link_libraries(WallpaperRenderer PRIVATE
  d3d11 dxgi dcomp windowscodecs ole32 shell32 user32 gdi32 shcore wtsapi32 advapi32 powrprof
)

set_target_properties(WallpaperRenderer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# ---------------------------------------------------------------- FFmpeg video backend (required)
# The only video path in the engine since 2026-10-07, when the Media Foundation decoder was deleted:
# it cost 290-313% of a core and 779-790 MB on the 4K60 clip this desktop runs, against 37% and
# 299-328 MB here, and it defined which formats the library would accept because the import probe used
# it. So this is a requirement, not an option: a tree without the SDK fails to configure with the one
# command that fixes it, rather than building an exe that quietly has no video.
#
# The SDK has to be the LGPL *shared* build - tools/get-ffmpeg.sh downloads that one and refuses
# anything configured with --enable-gpl or --enable-nonfree. Dynamic linking is not just a preference
# here: it is what the LGPL asks for, so the DLLs are staged next to the exe and can be swapped.
# Where the SDK is: -DWALLPAPER_FFMPEG_DIR=<path>, then the FFMPEG_DIR environment variable, then the
# build/ffm-lgpl that tools/get-ffmpeg.sh unpacks. A cache variable so -D actually reaches it - the
# first version of this block only read the environment, which made the message below a lie.
set(WALLPAPER_FFMPEG_DIR "$ENV{FFMPEG_DIR}" CACHE PATH "LGPL shared FFmpeg build to link against")
if(NOT WALLPAPER_FFMPEG_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/build/ffm-lgpl/include/libavcodec/avcodec.h")
  set(WALLPAPER_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/build/ffm-lgpl" CACHE PATH "" FORCE)
endif()
if(NOT WALLPAPER_FFMPEG_DIR OR NOT EXISTS "${WALLPAPER_FFMPEG_DIR}/include/libavcodec/avcodec.h")
  message(FATAL_ERROR
    "FFmpeg (LGPL shared build) is required to build WallpaperRenderer - video wallpapers are decoded "
    "with libavcodec and there is no second backend any more. Run `bash tools/get-ffmpeg.sh`, or pass "
    "-DWALLPAPER_FFMPEG_DIR=<path> / set FFMPEG_DIR to an existing LGPL shared build "
    "(currently: '${WALLPAPER_FFMPEG_DIR}').")
endif()
message(STATUS "FFmpeg video backend: ${WALLPAPER_FFMPEG_DIR}")
target_include_directories(WallpaperRenderer PRIVATE "${WALLPAPER_FFMPEG_DIR}/include")
target_link_directories(WallpaperRenderer PRIVATE "${WALLPAPER_FFMPEG_DIR}/lib")
target_link_libraries(WallpaperRenderer PRIVATE avformat avcodec avutil swscale)
# The transitive closure of the four libraries we link, which is NOT the same list: dumpbin says
# avcodec-63.dll needs swresample-7.dll even though we never call swresample, and once the exe has
# import libs for any of these the loader resolves them at process start - so a missing one kills
# the renderer before it can even open its log file. avdevice/avfilter are genuinely unreferenced.
foreach(lib avcodec avformat avutil swscale swresample)
  file(GLOB dll "${WALLPAPER_FFMPEG_DIR}/bin/${lib}-*.dll")
  foreach(d ${dll})
    add_custom_command(TARGET WallpaperRenderer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${d}" "$<TARGET_FILE_DIR:WallpaperRenderer>")
  endforeach()
endforeach()
# Licence text + a note on how to get the matching source are distribution obligations, not
# courtesy. The source is the pinned archive that tools/get-ffmpeg.sh fetches, and the note is
# generated from the binaries by tools/ffmpeg-notice.sh - so it is committed and staged under the
# name the shipped folder uses.
add_custom_command(TARGET WallpaperRenderer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${WALLPAPER_FFMPEG_DIR}/LICENSE.txt" "$<TARGET_FILE_DIR:WallpaperRenderer>/LICENSE-FFmpeg.txt")
if(EXISTS "${CMAKE_SOURCE_DIR}/licenses/FFmpeg.txt")
  add_custom_command(TARGET WallpaperRenderer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/licenses/FFmpeg.txt" "$<TARGET_FILE_DIR:WallpaperRenderer>/NOTICE-FFmpeg.txt")
else()
  message(WARNING "licenses/FFmpeg.txt is missing - run tools/ffmpeg-notice.sh; the shipped folder "
                  "must carry the notice that says which FFmpeg build this is and where its source is")
endif()

# The renderer reads Wallpapers/ and Shaders/ next to the exe, falling back two levels up,
# so a dev build works straight out of build/bin without an install step.
# ---------------------------------------------------------------- Qt settings window
# Optional: the renderer must still build on a machine without Qt.
list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_DIR}")
find_package(Qt6 QUIET COMPONENTS Quick Qml Gui Widgets ShaderTools)
if(Qt6_FOUND)
  set(CMAKE_AUTOMOC ON)
  qt_add_executable(Wallpaper WIN32
    Wallpaper/main.cpp
    resources/app.rc
    Wallpaper/Bridge.cpp
    Wallpaper/LivePreview.cpp
    Wallpaper/TrayMenu.cpp
    Engine/Core/Json.cpp
    Engine/Core/Log.cpp
    Engine/App/IPCClient.cpp
  )
  target_include_directories(Wallpaper PRIVATE "${CMAKE_SOURCE_DIR}")
  target_compile_definitions(Wallpaper PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(Wallpaper PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
  target_link_libraries(Wallpaper PRIVATE Qt6::Quick Qt6::Qml Qt6::Gui Qt6::Widgets)
  # The hero transition's fragment shader. Qt's own baker is the only thing that knows which
  # shader versions the scene graph asks for, so it is invoked through qt_add_shaders rather than
  # by hand - and the result is copied next to the QML instead of being embedded, because Main.qml
  # is loaded from disk on purpose (see main.cpp).
  qt_add_shaders(Wallpaper "wallpaper_qml_shaders"
    PREFIX "/shaders"
    FILES Wallpaper/shaders/HeroLiquid.frag
    OUTPUT_TARGETS wallpaper_shader_resources)
  add_custom_command(TARGET Wallpaper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_SOURCE_DIR}/Wallpaper/qml" "$<TARGET_FILE_DIR:Wallpaper>/qml"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_BINARY_DIR}/.qsb/Wallpaper/shaders/HeroLiquid.frag.qsb"
      "$<TARGET_FILE_DIR:Wallpaper>/qml/HeroLiquid.frag.qsb"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/resources/Wallpaper.ico" "$<TARGET_FILE_DIR:Wallpaper>/Wallpaper.ico"
    COMMENT "staging qml/, HeroLiquid.frag.qsb and Wallpaper.ico next to the settings window")
  set_target_properties(Wallpaper PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
  message(STATUS "Qt6 found: building Wallpaper settings window")
else()
  message(STATUS "Qt6 not found: skipping the Wallpaper settings window")
endif()

# Assets are staged next to the exe, which is the same directory the renderer searches first.
# "stage" is an ALL target so editing a .hlsl takes effect on the next build without a rebuild.
add_custom_target(stage ALL
  DEPENDS WallpaperRenderer
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Wallpapers" "$<TARGET_FILE_DIR:WallpaperRenderer>/Wallpapers"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Shaders" "$<TARGET_FILE_DIR:WallpaperRenderer>/Shaders"
  COMMENT "staging Wallpapers/ and Shaders/ next to the exe")
