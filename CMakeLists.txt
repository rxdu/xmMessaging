cmake_minimum_required(VERSION 3.16)
project(xmMessaging VERSION 0.0.1 LANGUAGES CXX)

# P0a: the API headers exist (include/xmmessaging/, derived from the D1-D18
# deltas in docs/scenarios.md) and the wish-code scenario suite compiles
# against them as an OBJECT library — compile-only, no linking, so the
# declared-but-undefined API is exercised exactly as far as P0a promises.
# Behavior (the in-process reach) arrives at P0b.
option(BUILD_TESTING "Build tests" OFF)
option(XMMESSAGING_BUILD_SCENARIOS "Build the scenario acceptance suite" ON)
# Backend seam (ADR 0006 / ADR 0004 pattern): one option per engine. The
# external-dependency backends default off; the dependency-free POSIX shm
# fallback defaults on (design.md "Backend seam"). The in-process reach is
# always present and dependency-free.
option(XMMESSAGING_WITH_ICEORYX2  "Build the iceoryx2 inter-process backend" OFF)
option(XMMESSAGING_WITH_ZENOH     "Build the Zenoh inter-host backend"       OFF)
option(XMMESSAGING_WITH_POSIX_SHM "Build the POSIX shm fallback backend"     ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

## Foundation dependency: xmBase (clock, telemetry context, common types).
## In-tree target > installed > bundled submodule — same resolution order as
## xmTelemetry/xmDriver.
if (NOT TARGET xmotion::xmBase)
  find_package(xmBase 0.3.0 QUIET)
  if (NOT xmBase_FOUND)
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xmBase/CMakeLists.txt)
      message(STATUS "xmMessaging: using bundled third_party/xmBase")
      # EXCLUDE_FROM_ALL: the bundled copy is a build convenience only — its
      # install rules must not leak into this project's install tree or deb
      # payload (xmBase ships separately as libxmotion-base).
      add_subdirectory(third_party/xmBase EXCLUDE_FROM_ALL)
      # Vendored headers are not first-party code: treat their include dirs
      # as SYSTEM so warnings never gate on the pinned dependency.
      set_target_properties(xmBase PROPERTIES
          INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
          $<TARGET_PROPERTY:xmBase,INTERFACE_INCLUDE_DIRECTORIES>)
    else ()
      message(FATAL_ERROR "xmMessaging: xmBase not found — install xmBase or init the third_party/xmBase submodule")
    endif ()
  endif ()
endif ()

## The portable API surface. INTERFACE (header-only) at P0a: every verb is
## declared, nothing is defined; the in-process reach turns this into a
## compiled library at P0b.
add_library(xmmessaging INTERFACE)
target_include_directories(xmmessaging INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(xmmessaging INTERFACE xmotion::xmBase)
target_compile_features(xmmessaging INTERFACE cxx_std_17)
add_library(xmotion::xmmessaging ALIAS xmmessaging)

## Backend seam status (engines arrive at P1/P1b/P2; the options exist now so
## application build scripts can already pin their intent).
if (XMMESSAGING_WITH_POSIX_SHM)
  message(STATUS "xmMessaging: POSIX shm fallback backend enabled (implementation lands at P1b)")
endif ()
if (XMMESSAGING_WITH_ICEORYX2)
  message(STATUS "xmMessaging: iceoryx2 backend enabled (implementation lands at P1)")
endif ()
if (XMMESSAGING_WITH_ZENOH)
  message(STATUS "xmMessaging: Zenoh backend enabled (implementation lands at P2)")
endif ()

if (XMMESSAGING_BUILD_SCENARIOS)
  ## The wish-code suite (docs/scenarios.md M1-M14), compiled UNMODIFIED as
  ## an OBJECT library: compile-only, no linking, so declarations suffice —
  ## the same staging xmTelemetry used for its P0a. scenario_support.hpp is
  ## force-included to supply the scenario-side helper declarations without
  ## touching the wish-code files (they ARE the spec).
  add_library(xmmessaging_scenarios OBJECT
      test/scenarios/m1_planning_control_loop.cpp
      test/scenarios/m2_late_join.cpp
      test/scenarios/m3_backpressure.cpp
      test/scenarios/m5_request_response.cpp
      test/scenarios/m6_reach_transparency.cpp
      test/scenarios/m7_trace_continuity.cpp
      test/scenarios/m13_pipeline_accounting.cpp
      test/scenarios/m14_stack_startup.cpp)
  target_link_libraries(xmmessaging_scenarios PRIVATE xmotion::xmmessaging)
  target_compile_options(xmmessaging_scenarios PRIVATE
      -Wall -Wextra
      -include ${CMAKE_CURRENT_SOURCE_DIR}/test/scenarios/scenario_support.hpp)
  ## M1 and M13 define TrajectoryHead / StateEstimate scenario-locally (the
  ## wish-code convention); the support header defines the identical layout
  ## for the other TUs that share them (M6/M7/M14).
  set_source_files_properties(test/scenarios/m1_planning_control_loop.cpp
      PROPERTIES COMPILE_DEFINITIONS XMMSG_SCENARIO_DEFINES_TRAJECTORY_HEAD)
  set_source_files_properties(test/scenarios/m13_pipeline_accounting.cpp
      PROPERTIES COMPILE_DEFINITIONS XMMSG_SCENARIO_DEFINES_STATE_ESTIMATE)
  ## Per-file scoping of two known-noise GCC diagnostics on deliberate
  ## wish-code constructs (the files are the spec and stay untouched):
  ## - M3 publishes `{.seq = i}` with the payload's other members
  ##   deliberately value-initialized (designated-initializer omission);
  ## - M6's MakeDomain returns from an exhaustive switch over its own Reach
  ##   enum with no default (GCC cannot see the coverage).
  set_source_files_properties(test/scenarios/m3_backpressure.cpp
      PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
  set_source_files_properties(test/scenarios/m6_reach_transparency.cpp
      PROPERTIES COMPILE_OPTIONS -Wno-return-type)
endif ()

message(STATUS "xmMessaging: P0a — API headers + compile-only scenario suite; see docs/scenarios.md")
