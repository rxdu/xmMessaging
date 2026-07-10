# test/link/check_ldd.cmake — M8-A3: the dependency-closure gate.
#
# Runs `ldd` on the lib-only link-test binary and fails if the closure
# references any transport backend (iceoryx2 / zenoh). Trivially true today
# (no backend is compiled at P0b); the point is the PERMANENT gate — when a
# backend option is introduced by mistake into the default closure, this is
# the test that catches it (docs/scenarios.md M8-A3).
#
# Usage: cmake -DBINARY=<path> -P check_ldd.cmake

if (NOT DEFINED BINARY)
  message(FATAL_ERROR "check_ldd.cmake: pass -DBINARY=<path to executable>")
endif ()

execute_process(COMMAND ldd "${BINARY}"
    OUTPUT_VARIABLE ldd_output
    ERROR_VARIABLE ldd_error
    RESULT_VARIABLE ldd_result)

if (NOT ldd_result EQUAL 0)
  message(FATAL_ERROR "check_ldd.cmake: ldd failed on ${BINARY}: ${ldd_error}")
endif ()

string(TOLOWER "${ldd_output}" ldd_lower)
# iceoryx2 ships libiox2* / libiceoryx2*; zenoh ships libzenoh*.
foreach (forbidden iceoryx iox zenoh)
  if (ldd_lower MATCHES "${forbidden}")
    message(FATAL_ERROR
        "M8-A3 VIOLATED: lib-only binary's dependency closure references "
        "'${forbidden}':\n${ldd_output}")
  endif ()
endforeach ()

message(STATUS "M8-A3: dependency closure clean (no iceoryx2/zenoh):")
message(STATUS "${ldd_output}")
