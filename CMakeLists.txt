cmake_minimum_required(VERSION 3.16)
project(xmMessaging VERSION 0.1.0 LANGUAGES CXX)

# P0b (part 1): the in-process reach is real — xmmessaging is a compiled
# STATIC library (seqlock LatestSlot, SPSC BoundedQueue, typed Domain
# registry; see include/xmmsg/detail/ and src/), and the behavioral
# suite (test/behavioral/, gated by BUILD_TESTING) asserts the M1/M2/M3/M14
# acceptance criteria from docs/scenarios.md. The wish-code scenario suite
# still compiles UNMODIFIED as an OBJECT library (it is the API spec).
option(BUILD_TESTING "Build tests" OFF)
option(XMMESSAGING_BUILD_SCENARIOS "Build the scenario acceptance suite" ON)
option(XMMESSAGING_BUILD_BENCH "Build the M9 benchmark tier (bench/)" OFF)
# The R5 introspection CLI (tools/xmmsg): part of the shipped payload, so it
# defaults ON — unlike bench/scenarios it is a user-facing deliverable.
option(XMMESSAGING_BUILD_TOOLS "Build the xmmsg introspection CLI (R5)" ON)
# Warnings-as-errors is a CI gate, not a local default: local builds keep
# -Wall -Wextra so a new compiler's novel warnings never block local work;
# CI passes -DXMMESSAGING_WERROR=ON (telemetry's XMOTION_WERROR pattern).
option(XMMESSAGING_WERROR "Treat warnings as errors on first-party targets" OFF)
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

## Sanitizer builds (M1-A2 requires the behavioral suite TSan-clean).
## Applied globally — including vendored xmBase and googletest — because a
## TSan process must have every synchronization site instrumented or the
## race detector reports false positives on the seam.
set(XMMESSAGING_SANITIZE "" CACHE STRING
    "Build everything with a sanitizer: '' (off), 'thread', or 'address'")
if (XMMESSAGING_SANITIZE STREQUAL "thread")
  # -Wno-tsan: GCC >= 13 warns that atomic_thread_fence is not instrumented
  # by TSan. The seqlock is deliberately the Boehm construction — relaxed
  # ATOMIC data words + fences — so TSan's race detection is sound through
  # the atomic accesses without modeling the fences; the warning flags a
  # known, accepted limitation, and under XMMESSAGING_WERROR it would
  # otherwise break the sanitizer leg (see latest_slot.hpp ordering notes).
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -Wno-tsan)
  add_link_options(-fsanitize=thread)
elseif (XMMESSAGING_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address)
elseif (NOT XMMESSAGING_SANITIZE STREQUAL "")
  message(FATAL_ERROR "xmMessaging: unknown XMMESSAGING_SANITIZE value '${XMMESSAGING_SANITIZE}'")
endif ()

include(GNUInstallDirs)

## First-party warning flags (vendored code is SYSTEM/excluded and never
## gates): every first-party target uses this one list.
set(XMMESSAGING_WARNINGS -Wall -Wextra)
if (XMMESSAGING_WERROR)
  list(APPEND XMMESSAGING_WARNINGS -Werror)
endif ()

## Foundation dependency: xmBase (clock, telemetry context, common types).
## In-tree target > installed > bundled submodule — same resolution order as
## xmTelemetry/xmDriver.
if (NOT TARGET xmotion::xmBase)
  find_package(xmBase 0.4.0 QUIET)
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

find_package(Threads REQUIRED)

## The portable API surface + the in-process reach (P0b): a compiled STATIC
## library. Templates (the typed verbs) live in the headers; the non-template
## core (Domain lifecycle, registry, Endpoint base) is compiled here.
add_library(xmmessaging STATIC
    src/domain.cpp)
target_include_directories(xmmessaging PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(xmmessaging PUBLIC xmotion::xmBase Threads::Threads)
target_compile_features(xmmessaging PUBLIC cxx_std_17)
target_compile_options(xmmessaging PRIVATE ${XMMESSAGING_WARNINGS})
add_library(xmotion::xmmessaging ALIAS xmmessaging)

## Backend seam status (iceoryx2/Zenoh arrive at P1/P2; the options exist now
## so application build scripts can already pin their intent).
if (XMMESSAGING_WITH_POSIX_SHM)
  if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "xmMessaging: POSIX shm fallback backend requires Linux (futex/shm_open) — disabled on ${CMAKE_SYSTEM_NAME}")
  else ()
    ## P1b: dependency-free — kernel primitives only (shm_open + futex).
    ## PUBLIC: the typed verbs live in headers, so consumers must see the
    ## same backend gate the library was built with.
    target_compile_definitions(xmmessaging PUBLIC XMMESSAGING_HAS_POSIX_SHM=1)
    message(STATUS "xmMessaging: POSIX shm fallback backend enabled (P1b)")
  endif ()
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
      ${XMMESSAGING_WARNINGS}
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

## R5/M10: the xmmsg introspection CLI. Added BEFORE the test tree so the
## M10/M11 behavioral tests can locate the binary (they run it as a real
## subprocess — the CLI is the deliverable under test).
if (XMMESSAGING_BUILD_TOOLS)
  add_subdirectory(tools/xmmsg)
endif ()

if (BUILD_TESTING)
  enable_testing()
  ## googletest: same family pattern as xmTelemetry (third_party submodule,
  ## pinned at v1.14.0). Until the submodule is added to this repo, fall
  ## back to FetchContent of the same pinned release. xmBase may already
  ## have brought a gtest target in via its own third_party.
  if (NOT TARGET gtest)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt)
      add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
    else ()
      message(STATUS "xmMessaging: third_party/googletest not present — fetching pinned v1.14.0")
      include(FetchContent)
      FetchContent_Declare(googletest
          URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
          URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
          DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
      FetchContent_MakeAvailable(googletest)
    endif ()
  endif ()
  add_subdirectory(test/behavioral)
  ## M8: lib-only link test + dependency-closure gate (test/link/).
  add_subdirectory(test/link)
endif ()

## M9: the benchmark tier (bench/). Opt-in — a plain library consumer never
## builds it; `scripts/bench.sh` is the one-command entry point (M9-A1).
if (XMMESSAGING_BUILD_BENCH)
  add_subdirectory(bench)
endif ()

## Installation setup (same conventions as xmBase/xmTelemetry: lib/, bin/,
## include/, lib/cmake/<name>). Works standalone with the test/scenario
## options OFF: a plain `cmake -B build && cmake --install build` installs
## the static library, the public headers (incl. detail/ — the typed verbs
## are header templates), and the xmmsg CLI (tools/xmmsg/CMakeLists.txt).
## Test binaries, scenarios, bench, and the bundled third_party/xmBase
## (EXCLUDE_FROM_ALL — it ships separately as libxmotion-base) are never
## installed.
install(TARGETS xmmessaging EXPORT xmMessagingTargets
    LIBRARY DESTINATION lib ARCHIVE DESTINATION lib RUNTIME DESTINATION bin
    INCLUDES DESTINATION include)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# export target configuration
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    xmMessagingConfigVersion.cmake
    VERSION "${${PROJECT_NAME}_VERSION_MAJOR}.${${PROJECT_NAME}_VERSION_MINOR}.${${PROJECT_NAME}_VERSION_PATCH}"
    COMPATIBILITY SameMajorVersion)

install(EXPORT xmMessagingTargets
    FILE xmMessagingTargets.cmake
    NAMESPACE xmotion::
    DESTINATION lib/cmake/xmMessaging)

configure_file(cmake/xmMessagingConfig.cmake.in xmMessagingConfig.cmake @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/xmMessagingConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/xmMessagingConfigVersion.cmake"
    DESTINATION lib/cmake/xmMessaging)

# Apache-2.0: the LICENSE ships as the package copyright doc (family
# pattern; the /opt/xmotion packaging prefix keeps it out of Debian's
# /usr/share/doc policy path), with the Apache NOTICE alongside it.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION share/doc/libxmotion-messaging RENAME copyright)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
    DESTINATION share/doc/libxmotion-messaging)

# Packaging support (R1: deb packaging follows the family pattern —
# xmBase/xmTelemetry conventions: libxmotion-* naming, /opt/xmotion prefix,
# one package carrying lib + headers + tools).
set(CPACK_DEBIAN_PACKAGE_NAME "libxmotion-messaging")
set(CPACK_PACKAGE_VENDOR "Ruixiang Du")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "xmMessaging - the XMotion application messaging layer: typed pub/sub and request/response across reaches, with the xmmsg introspection CLI")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/xmotion")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
# The declared xmBase floor — same pin as find_package/find_dependency.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libxmotion-base (>= 0.4.0)")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Ruixiang Du (ruixiang.du@gmail.com)")
set(CPACK_SOURCE_IGNORE_FILES .git dist .*build.* /\\\\.DS_Store)
include(CPack)

message(STATUS "xmMessaging: v0.1 — in-process + POSIX-shm reaches, behavioral suite (BUILD_TESTING); see docs/scenarios.md")
