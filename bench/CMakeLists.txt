# M9 benchmark tier (docs/scenarios.md M9, docs/design.md R4). One binary,
# one translation unit — the family alloc probe (xmbase/testing) replaces
# the global allocation functions and must be defined in exactly one TU.
# Harness + probe come from xmBase 0.5.0 (ADR 0007 W2; unified FROM this
# suite's local copies at W1), reached through the xmotion::xmmessaging
# PUBLIC xmBase include dirs.
# Entry point: scripts/bench.sh (configures build-bench at -O2 -DNDEBUG,
# runs, writes bench/results/<timestamp>.json, compares vs reference.json).

add_executable(xmmsg_bench xmmsg_bench.cpp)
target_link_libraries(xmmsg_bench PRIVATE xmotion::xmmessaging)
target_compile_options(xmmsg_bench PRIVATE ${XMMESSAGING_WARNINGS})
