cmake_minimum_required(VERSION 3.24)
project(stccfix LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "STCC は 32bit プロセスです。-A Win32 で構成してください")
endif()

# CRT を静的リンクし、ゲームフォルダに DLL 1 個を置くだけで動くようにする
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------- MinHook (third_party/minhook, BSD-2)
set(MINHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook)
add_library(minhook STATIC
  ${MINHOOK_DIR}/src/buffer.c
  ${MINHOOK_DIR}/src/hook.c
  ${MINHOOK_DIR}/src/trampoline.c
  ${MINHOOK_DIR}/src/hde/hde32.c
)
target_include_directories(minhook PUBLIC ${MINHOOK_DIR}/include)

# ---------------------------------------------------------------- stccfix (dinput.dll)
add_library(stccfix SHARED
  src/stccfix/common.h
  src/stccfix/log.cpp
  src/stccfix/config.cpp
  src/stccfix/patch.cpp
  src/stccfix/proxy_dinput.cpp
  src/stccfix/vtable_hook.h
  src/stccfix/vtable_hook.cpp
  src/stccfix/hooks_ddraw.cpp
  src/stccfix/hooks_dinput.cpp
  src/stccfix/hooks_game.cpp
  src/stccfix/dllmain.cpp
  src/stccfix/dinput.def
)
# ゲームは DINPUT.dll を静的 import しているので、この名前でゲームフォルダに置く
set_target_properties(stccfix PROPERTIES OUTPUT_NAME dinput)
target_compile_features(stccfix PRIVATE cxx_std_20)
target_compile_definitions(stccfix PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(stccfix PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(stccfix PRIVATE minhook dxguid)
