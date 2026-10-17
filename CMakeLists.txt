cmake_minimum_required(VERSION 3.20)
project(EmberKV VERSION 1.0.0 DESCRIPTION "A Redis-compatible in-memory key-value store" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(EMBERKV_BUILD_TESTS "Build the test suite (downloads GoogleTest)" ON)
option(EMBERKV_BUILD_BENCH "Build the benchmark" ON)
option(EMBERKV_WERROR "Treat compiler warnings as errors" OFF)
option(EMBERKV_FORCE_POLL "Use the portable poll() backend even on Linux" OFF)
set(EMBERKV_SANITIZE "" CACHE STRING "Sanitizers for GCC/Clang, e.g. address,undefined or thread")

find_package(Threads REQUIRED)

# Apple Clang before 17 (Xcode 16 and older) ships a libc++ that still keeps
# std::jthread and std::stop_token behind -fexperimental-library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
  add_compile_options(-fexperimental-library)
  add_link_options(-fexperimental-library)
endif()

# ---------------------------------------------------------------- warnings
add_library(emberkv_options INTERFACE)
if(MSVC)
  target_compile_options(emberkv_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  if(EMBERKV_WERROR)
    target_compile_options(emberkv_options INTERFACE /WX)
  endif()
else()
  target_compile_options(emberkv_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor
    -Wold-style-cast -Woverloaded-virtual -Wimplicit-fallthrough)
  if(EMBERKV_WERROR)
    target_compile_options(emberkv_options INTERFACE -Werror)
  endif()
endif()

if(EMBERKV_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=${EMBERKV_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${EMBERKV_SANITIZE})
endif()

# ------------------------------------------------------------------ library
add_library(emberkv
  src/commands.cpp
  src/config.cpp
  src/net.cpp
  src/resp.cpp
  src/server.cpp
  src/snapshot.cpp
  src/store.cpp
)
add_library(emberkv::emberkv ALIAS emberkv)
target_include_directories(emberkv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(emberkv PUBLIC Threads::Threads PRIVATE emberkv_options)
if(WIN32)
  target_link_libraries(emberkv PUBLIC ws2_32)
  target_compile_definitions(emberkv PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(EMBERKV_FORCE_POLL)
  target_compile_definitions(emberkv PRIVATE EMBERKV_FORCE_POLL)
endif()

# -------------------------------------------------------------- executables
add_executable(emberkv-server src/main.cpp)
target_link_libraries(emberkv-server PRIVATE emberkv emberkv_options)

add_executable(emberkv-cli tools/cli.cpp)
target_link_libraries(emberkv-cli PRIVATE emberkv emberkv_options)

if(EMBERKV_BUILD_BENCH)
  add_executable(emberkv-bench bench/bench_store.cpp)
  target_link_libraries(emberkv-bench PRIVATE emberkv emberkv_options)
endif()

# -------------------------------------------------------------------- tests
if(EMBERKV_BUILD_TESTS)
  # Use an installed GoogleTest (vcpkg, apt, brew) if there is one, else fetch it.
  find_package(GTest 1.12 CONFIG QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.15.2
      GIT_SHALLOW TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # match MSVC's default runtime
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  add_executable(emberkv-tests
    tests/test_commands.cpp
    tests/test_config.cpp
    tests/test_lru_cache.cpp
    tests/test_resp.cpp
    tests/test_server.cpp
    tests/test_snapshot.cpp
    tests/test_store.cpp
  )
  target_link_libraries(emberkv-tests PRIVATE emberkv emberkv_options GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(emberkv-tests DISCOVERY_TIMEOUT 30)
endif()
