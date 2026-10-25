cmake_minimum_required(VERSION 3.16)
project(moonsweepers_trainer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TRAINER_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string embedded in the executable")

# Static runtime: the exe runs on a clean Windows install without redistributables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

function(trainer_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

# Decimal parsing is portable and unit-tested on every platform.
enable_testing()
add_executable(decimal_tests tests/decimal_test.cpp src/decimal.cpp)
trainer_warnings(decimal_tests)
add_test(NAME decimal COMMAND decimal_tests)

if(WIN32)
  add_executable(moonsweepers-trainer
    src/main.cpp
    src/decimal.cpp
    src/process.cpp
    src/wallet.cpp)
  target_compile_definitions(moonsweepers-trainer PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    TRAINER_VERSION="${TRAINER_VERSION}")
  trainer_warnings(moonsweepers-trainer)
  if(NOT MSVC)
    target_link_options(moonsweepers-trainer PRIVATE -static)
  endif()
endif()
