cmake_minimum_required(VERSION 3.24)
project(SmartWallpaper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
  message(FATAL_ERROR "MSVC is required (C++20 + Win32 + D3D11)")
endif()

add_executable(WallpaperRenderer WIN32
  main.cpp
  Engine/Core/Log.cpp
  Engine/Core/Json.cpp
  Engine/Graphics/D3D11Device.cpp
  Engine/Graphics/Shader.cpp
  Engine/Graphics/Texture.cpp
  Engine/Graphics/ParticleSystem.cpp
  Engine/Graphics/D3D11Renderer.cpp
  Engine/Wallpaper/WallpaperPackage.cpp
  Engine/Wallpaper/WallpaperInstance.cpp
  Engine/Wallpaper/WallpaperManager.cpp
  Engine/Wallpaper/Thumbnailer.cpp
  Engine/Desktop/MonitorManager.cpp
  Engine/Desktop/WorkerW.cpp
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
target_link_libraries(WallpaperRenderer PRIVATE
  d3d11 dxgi dcomp windowscodecs ole32 shell32 user32 gdi32 shcore wtsapi32 advapi32 powrprof
)

set_target_properties(WallpaperRenderer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# The renderer reads Wallpapers/ and Shaders/ next to the exe, falling back two levels up,
# so a dev build works straight out of build/bin without an install step.
# ---------------------------------------------------------------- Qt settings window
# Optional: the renderer must still build on a machine without Qt.
list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_DIR}")
find_package(Qt6 QUIET COMPONENTS Quick Qml Gui Widgets)
if(Qt6_FOUND)
  set(CMAKE_AUTOMOC ON)
  qt_add_executable(SmartWallpaper WIN32
    Wallpaper/main.cpp
    Wallpaper/Bridge.cpp
    Wallpaper/LivePreview.cpp
    Engine/Core/Json.cpp
    Engine/Core/Log.cpp
    Engine/App/IPCClient.cpp
  )
  target_include_directories(SmartWallpaper PRIVATE "${CMAKE_SOURCE_DIR}")
  target_compile_definitions(SmartWallpaper PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(SmartWallpaper PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
  target_link_libraries(SmartWallpaper PRIVATE Qt6::Quick Qt6::Qml Qt6::Gui Qt6::Widgets)
  add_custom_command(TARGET SmartWallpaper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_SOURCE_DIR}/Wallpaper/qml" "$<TARGET_FILE_DIR:SmartWallpaper>/qml"
    COMMENT "staging qml/ next to the settings window")
  set_target_properties(SmartWallpaper PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
  message(STATUS "Qt6 found: building SmartWallpaper settings window")
else()
  message(STATUS "Qt6 not found: skipping the SmartWallpaper settings window")
endif()

# Assets are staged next to the exe, which is the same directory the renderer searches first.
# "stage" is an ALL target so editing a .hlsl takes effect on the next build without a rebuild.
add_custom_target(stage ALL
  DEPENDS WallpaperRenderer
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Wallpapers" "$<TARGET_FILE_DIR:WallpaperRenderer>/Wallpapers"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Shaders" "$<TARGET_FILE_DIR:WallpaperRenderer>/Shaders"
  COMMENT "staging Wallpapers/ and Shaders/ next to the exe")
