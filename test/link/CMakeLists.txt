# M8 link tier (docs/scenarios.md): the lib-only proof, as permanent gates.
#  - m8_libonly_runs        — the binary links ONLY xmotion::xmmessaging (no
#                             gtest) and exercises one in-process publish/take
#                             (M8-A1) + explicit unsupported-reach (M8-A2).
#  - m8_dependency_closure  — `ldd` closure carries no iceoryx2/zenoh (M8-A3).
#  - m8_default_config      — a FRESH default configure has both backend
#                             options OFF (M8-A1: backends strictly opt-in).
# The closure/default gates only make sense in a lib-only configuration; a
# build that deliberately enables a backend is asserting a different closure.

add_executable(xmmsg_libonly_link_test libonly_main.cpp)
target_link_libraries(xmmsg_libonly_link_test PRIVATE xmotion::xmmessaging)
target_compile_options(xmmsg_libonly_link_test PRIVATE ${XMMESSAGING_WARNINGS})

add_test(NAME m8_libonly_runs COMMAND xmmsg_libonly_link_test)
set_tests_properties(m8_libonly_runs PROPERTIES TIMEOUT 60)

if (NOT XMMESSAGING_WITH_ICEORYX2 AND NOT XMMESSAGING_WITH_ZENOH)
  add_test(NAME m8_dependency_closure
      COMMAND ${CMAKE_COMMAND}
          -DBINARY=$<TARGET_FILE:xmmsg_libonly_link_test>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_ldd.cmake)
  set_tests_properties(m8_dependency_closure PROPERTIES TIMEOUT 60)

  add_test(NAME m8_default_config
      COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
          -DPROBE_DIR=${CMAKE_CURRENT_BINARY_DIR}/default-config-probe
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_default_config.cmake)
  # A fresh configure resolves xmBase and probes the toolchain: allow time.
  set_tests_properties(m8_default_config PROPERTIES TIMEOUT 300)
endif ()
