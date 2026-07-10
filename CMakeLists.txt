cmake_minimum_required(VERSION 3.16)
project(xmMessaging VERSION 0.0.1 LANGUAGES CXX)

# P0.0: the scenario suite (docs/scenarios.md) is being wish-coded — it
# defines the API before any header exists. This file establishes the
# project shell and the backend option surface only; nothing builds yet.
# The scenario OBJECT library arrives at P0a (same staging as xmTelemetry).
option(BUILD_TESTING "Build tests" OFF)
option(XMMESSAGING_BUILD_SCENARIOS "Build the scenario acceptance suite" ON)
# Backend seam (ADR 0006 / ADR 0004 pattern): one option per engine, all
# default-off. The in-process reach is always present and dependency-free.
option(XMMESSAGING_WITH_ICEORYX2 "Build the iceoryx2 inter-process backend" OFF)
option(XMMESSAGING_WITH_ZENOH    "Build the Zenoh inter-host backend"       OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

message(STATUS "xmMessaging: P0.0 (wish-code) — no build targets yet; see docs/scenarios.md")
