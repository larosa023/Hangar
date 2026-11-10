# Build Hangar on Windows with Visual Studio 2022 (Desktop C++ workload):
#   cmake -S . -B build -A x64
#   cmake --build build --config Release
# Or cross-compile from Linux/macOS with ./build.sh (Zig).
cmake_minimum_required(VERSION 3.20)
project(Hangar LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, no redistributable needed

file(GLOB HANGAR_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(Hangar WIN32 ${HANGAR_SOURCES} res/hangar.rc)
target_compile_definitions(Hangar PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
if(MSVC)
  target_compile_options(Hangar PRIVATE /W4 /utf-8 /permissive-)
  target_link_options(Hangar PRIVATE /MANIFEST:NO /ENTRY:wWinMainCRTStartup)   # manifest comes from hangar.rc
endif()
target_link_libraries(Hangar PRIVATE d2d1 dwrite ole32 oleaut32 uuid dwmapi shcore shell32 user32 gdi32 advapi32 shlwapi windowscodecs comctl32 uxtheme winhttp bcrypt version)
set_target_properties(Hangar PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
