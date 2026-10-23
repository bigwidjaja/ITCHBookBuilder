cmake_minimum_required(VERSION 3.20)
project(itchbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ITCHBOOK_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ITCHBOOK_BUILD_TESTS "Build the GoogleTest suite" ON)
option(ITCHBOOK_PYTHON "Build the Python module (pip install . turns this on)" OFF)
option(ITCHBOOK_BUILD_BENCH "Build the Google Benchmark microbenchmarks" OFF)

# The library holds the real code. Later front ends link this same target.
add_library(itchbook_core STATIC
  src/core.cpp
  src/messages.cpp
  src/format.cpp
  src/dump.cpp
  src/symbols.cpp
  src/stats.cpp
  src/book_v1.cpp
  src/replay.cpp
  src/snapshot.cpp
)
target_include_directories(itchbook_core PUBLIC include)
# The Python module is a shared library, and a static library linked into a
# shared one must be compiled as position-independent code.
set_target_properties(itchbook_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(ITCHBOOK_SANITIZE)
  # PUBLIC so the CLI and tests compile and link with the same sanitizers.
  target_compile_options(itchbook_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(itchbook_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

# The CLI is a thin executable. PRIVATE means only itchbook uses the library
# this way; the library does not re-export that link to anyone else.
add_executable(itchbook
  tools/main.cpp
)
target_link_libraries(itchbook PRIVATE itchbook_core)

include(FetchContent)

if(ITCHBOOK_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(itchbook_tests
    tests/core_test.cpp
  )
  target_link_libraries(itchbook_tests PRIVATE itchbook_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(itchbook_tests)
endif()

if(ITCHBOOK_BUILD_BENCH)
  FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googlebenchmark)

  add_executable(itchbook_bench bench/book_bench.cpp)
  target_link_libraries(itchbook_bench PRIVATE itchbook_core benchmark::benchmark)
endif()

if(ITCHBOOK_PYTHON)
  # Development.Module is just the headers needed to build an extension module.
  find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module)
  FetchContent_Declare(
    pybind11
    URL https://github.com/pybind/pybind11/archive/refs/tags/v2.13.6.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(pybind11)

  # _itchbook is the compiled half of the itchbook package in python/itchbook.
  pybind11_add_module(_itchbook python/src/bindings.cpp)
  target_link_libraries(_itchbook PRIVATE itchbook_core)
  install(TARGETS _itchbook LIBRARY DESTINATION itchbook)
endif()
