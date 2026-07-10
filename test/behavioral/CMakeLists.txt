# Behavioral acceptance suite (P0b in-process + P1b POSIX-shm): each binary
# asserts one scenario's acceptance criteria from docs/scenarios.md. Separate
# binaries so a TSan run (XMMESSAGING_SANITIZE=thread) reports per-scenario
# and so `ctest -R m1` targets one contract.
#
# P1b: m1/m2/m3 gain cross-process shm legs, m4 (crash recovery) and
# wire_schema (§5 conformance vectors) join, and m6 gains its PosixShm
# ReachCase. The cross-process legs fork+exec shm_test_helper — a
# purpose-built single-threaded child (cleaner than forking the gtest
# binary under TSan/ASan). TSan cannot see cross-process shm races (noted
# in shm_test_support.hpp); run the shm suites under ASan too
# (XMMESSAGING_SANITIZE=address), which catches mapping/bounds bugs.

include(GoogleTest)

set(XMMSG_SHM_ENABLED FALSE)
if (XMMESSAGING_WITH_POSIX_SHM AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(XMMSG_SHM_ENABLED TRUE)
  add_executable(shm_test_helper shm_helper_main.cpp)
  target_link_libraries(shm_test_helper PRIVATE xmotion::xmmessaging)
  target_compile_options(shm_test_helper PRIVATE ${XMMESSAGING_WARNINGS})
endif ()

foreach(scenario IN ITEMS m1 m2 m3 m4 m5 m6 m7 m13 m14)
  add_executable(${scenario}_behavioral_test ${scenario}_behavioral_test.cpp)
  target_link_libraries(${scenario}_behavioral_test PRIVATE
      xmotion::xmmessaging gtest gtest_main)
  target_compile_options(${scenario}_behavioral_test PRIVATE ${XMMESSAGING_WARNINGS})
  if (XMMSG_SHM_ENABLED)
    # The cross-process legs locate the helper binary at run time.
    target_compile_definitions(${scenario}_behavioral_test PRIVATE
        XMMSG_SHM_HELPER_PATH="$<TARGET_FILE:shm_test_helper>")
    add_dependencies(${scenario}_behavioral_test shm_test_helper)
  endif ()
  gtest_discover_tests(${scenario}_behavioral_test PROPERTIES TIMEOUT 180)
endforeach()

# R6 wire-contract §4/§5 conformance (hash + canonical description).
add_executable(wire_schema_test wire_schema_test.cpp)
target_link_libraries(wire_schema_test PRIVATE
    xmotion::xmmessaging gtest gtest_main)
target_compile_options(wire_schema_test PRIVATE ${XMMESSAGING_WARNINGS})
gtest_discover_tests(wire_schema_test PROPERTIES TIMEOUT 60)
