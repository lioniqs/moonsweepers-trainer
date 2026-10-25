cmake_minimum_required(VERSION 3.16)
project(moonsweepers_trainer VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TRAINER_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string embedded in the executables")

# Static runtime: the executables run on a clean Windows install without redistributables.
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
  # Everything that talks to the game process.
  add_library(trainer_core STATIC
    src/decimal.cpp
    src/process.cpp
    src/il2cpp.cpp
    src/game.cpp
    src/storage.cpp
    src/wallet.cpp)
  target_compile_definitions(trainer_core PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    TRAINER_VERSION="${TRAINER_VERSION}")
  target_include_directories(trainer_core PUBLIC src)
  trainer_warnings(trainer_core)

  # Window with wallet, free teleport and the flag/site navigator.
  add_executable(moonsweepers-trainer WIN32 src/gui.cpp)
  target_compile_definitions(moonsweepers-trainer PRIVATE UNICODE _UNICODE)
  target_link_libraries(moonsweepers-trainer PRIVATE trainer_core comctl32)
  trainer_warnings(moonsweepers-trainer)

  # The same window without any write access: wallet and prices are gone, only reading and navigation.
  add_executable(moonsweepers-navigator WIN32 src/gui.cpp)
  target_compile_definitions(moonsweepers-navigator PRIVATE UNICODE _UNICODE NAVIGATOR_ONLY)
  target_link_libraries(moonsweepers-navigator PRIVATE trainer_core comctl32)
  trainer_warnings(moonsweepers-navigator)

  # Command-line version (scripting, --set / --freeze).
  add_executable(moonsweepers-trainer-cli src/main.cpp)
  target_link_libraries(moonsweepers-trainer-cli PRIVATE trainer_core)
  trainer_warnings(moonsweepers-trainer-cli)

  if(NOT MSVC)
    target_link_options(moonsweepers-trainer PRIVATE -static -mwindows)
    target_link_options(moonsweepers-navigator PRIVATE -static -mwindows)
    target_link_options(moonsweepers-trainer-cli PRIVATE -static)
  endif()
endif()
