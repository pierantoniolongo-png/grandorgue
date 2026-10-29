# Organ engine for Android (and a desktop test tool).
# License GPL-2.0 or later.
cmake_minimum_required(VERSION 3.18)
project(organengine CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# WavPack: the system library on the desktop, built from source on Android
find_package(PkgConfig QUIET)
if(NOT ANDROID AND PkgConfig_FOUND)
  pkg_check_modules(WAVPACK wavpack)
endif()
if(WAVPACK_FOUND)
  set(WAVPACK_LIB ${WAVPACK_LINK_LIBRARIES})
  # the system header lives in <prefix>/include/wavpack/wavpack.h
  find_path(WAVPACK_INC wavpack.h PATH_SUFFIXES wavpack HINTS ${WAVPACK_INCLUDE_DIRS})
else()
  include(FetchContent)
  set(WAVPACK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(WAVPACK_INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(WAVPACK_INSTALL_CMAKE_MODULE OFF CACHE BOOL "" FORCE)
  set(WAVPACK_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(WAVPACK_BUILD_COOLEDIT_PLUGIN OFF CACHE BOOL "" FORCE)
  set(WAVPACK_BUILD_WINAMP_PLUGIN OFF CACHE BOOL "" FORCE)
  set(WAVPACK_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(WAVPACK_ENABLE_LIBCRYPTO OFF CACHE BOOL "" FORCE)
  set(WAVPACK_ENABLE_ASM OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    wavpack
    GIT_REPOSITORY https://github.com/dbry/WavPack.git
    GIT_TAG 5.7.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(wavpack)
  set(WAVPACK_LIB wavpack)
  set(WAVPACK_INC ${wavpack_SOURCE_DIR}/include)
endif()

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_library(organcore STATIC
  engine/odf.cpp
  engine/source.cpp
  engine/wave.cpp
  engine/organ.cpp
  engine/engine.cpp)
target_include_directories(organcore PUBLIC engine ${WAVPACK_INC})
target_link_libraries(organcore PUBLIC ${WAVPACK_LIB} ZLIB::ZLIB Threads::Threads)

if(ANDROID)
  add_library(organapp SHARED jni_bridge.cpp audio_aaudio.cpp)
  target_link_libraries(organapp PRIVATE organcore aaudio log android)
else()
  add_executable(render_test tools/render_test.cpp)
  target_link_libraries(render_test PRIVATE organcore)
endif()
