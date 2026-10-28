cmake_minimum_required(VERSION 3.20)
project(lsm_kv_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KV_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Applied globally (before fetching GoogleTest) so every target, including
# gtest itself, is instrumented consistently.
if(KV_SANITIZE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "KV_SANITIZE requires GCC or Clang")
  endif()
  set(KV_SANITIZER_FLAGS
    -fsanitize=address,undefined
    -fno-sanitize-recover=undefined  # make UB a hard failure, not just a log line
    -fno-omit-frame-pointer)
  add_compile_options(${KV_SANITIZER_FLAGS})
  add_link_options(${KV_SANITIZER_FLAGS})
endif()

# ---- Library ----------------------------------------------------------------
# INTERFACE for now because src/ contains only headers. Once you add a .cpp,
# change to:  add_library(kv STATIC src/core/<file>.cpp ...)
# and change INTERFACE below to PUBLIC.
add_library(kv INTERFACE)
target_include_directories(kv INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(kv INTERFACE cxx_std_20)

# ---- Tests ------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_subdirectory(tests/harness)
endif()
