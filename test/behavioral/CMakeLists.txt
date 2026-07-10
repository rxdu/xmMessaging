# Behavioral acceptance suite (P0b, in-process reach): each binary asserts
# one scenario's acceptance criteria from docs/scenarios.md. Separate
# binaries so a TSan run (XMMESSAGING_SANITIZE=thread) reports per-scenario
# and so `ctest -R m1` targets one contract.

include(GoogleTest)

foreach(scenario IN ITEMS m1 m2 m3 m14)
  add_executable(${scenario}_behavioral_test ${scenario}_behavioral_test.cpp)
  target_link_libraries(${scenario}_behavioral_test PRIVATE
      xmotion::xmmessaging gtest gtest_main)
  target_compile_options(${scenario}_behavioral_test PRIVATE -Wall -Wextra)
  gtest_discover_tests(${scenario}_behavioral_test PROPERTIES TIMEOUT 180)
endforeach()
