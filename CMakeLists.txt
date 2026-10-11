cmake_minimum_required(VERSION 3.20)
project(mini_oems
  VERSION 0.1.0
  DESCRIPTION "FIX 4.4 order gateway with pre-trade risk checks"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MINI_OEMS_BUILD_TESTS "Build the unit and integration tests" ON)

# MinGW: link the C++ runtime statically so the binaries run without MSYS2's DLLs on PATH.
if(MINGW)
  add_link_options(-static)
endif()

# Compiler warnings for this project's own code (third-party code is not affected).
add_library(oems_options INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(oems_options INTERFACE -Wall -Wextra -Wpedantic -Wconversion)
endif()

# QuickFIX: pinned and built by scripts/build_quickfix.sh. Search only that install,
# so a different QuickFIX installed elsewhere on the machine can never be picked up.
set(MINI_OEMS_QUICKFIX_PREFIX "${PROJECT_SOURCE_DIR}/build/_deps/quickfix-install"
  CACHE PATH "Install prefix of the pinned QuickFIX build")
find_package(quickfix CONFIG REQUIRED
  PATHS "${MINI_OEMS_QUICKFIX_PREFIX}/share/cmake/quickfix" "${MINI_OEMS_QUICKFIX_PREFIX}"
  NO_DEFAULT_PATH)

# The core: order types, lifecycle, risk and matching. Plain C++ with no FIX
# dependency, so all of it can be tested without sockets or sessions.
add_library(oems_core
  src/types.cpp
  src/order.cpp
)
target_include_directories(oems_core PUBLIC include)
target_link_libraries(oems_core PRIVATE oems_options)

if(MINI_OEMS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
