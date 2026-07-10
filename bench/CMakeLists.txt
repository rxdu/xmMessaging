# M9 benchmark tier (docs/scenarios.md M9, docs/design.md R4). One binary,
# one translation unit — the behavioral suite's alloc probe replaces the
# global allocation functions and must be defined in exactly one TU.
# Entry point: scripts/bench.sh (configures build-bench at -O2 -DNDEBUG,
# runs, writes bench/results/<timestamp>.json, compares vs reference.json).

add_executable(xmmsg_bench xmmsg_bench.cpp)
target_link_libraries(xmmsg_bench PRIVATE xmotion::xmmessaging)
# alloc_probe.hpp is shared with the behavioral suite (one probe, one
# methodology — telemetry S1 pattern).
target_include_directories(xmmsg_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../test/behavioral)
target_compile_options(xmmsg_bench PRIVATE ${XMMESSAGING_WARNINGS})
