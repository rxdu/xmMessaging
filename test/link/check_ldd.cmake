# test/link/check_ldd.cmake — M8-A3: the dependency-closure gate.
#
# Runs the platform's shared-library lister on the lib-only link-test binary
# (`ldd`, or `otool -L` on macOS, which has no ldd) and fails if the closure
# references any transport backend (iceoryx2 / zenoh). Trivially true today
# (no backend is compiled at P0b); the point is the PERMANENT gate — when a
# backend option is introduced by mistake into the default closure, this is
# the test that catches it (docs/scenarios.md M8-A3).
#
# Usage: cmake -DBINARY=<path> -P check_ldd.cmake

if (NOT DEFINED BINARY)
  message(FATAL_ERROR "check_ldd.cmake: pass -DBINARY=<path to executable>")
endif ()

# macOS has no ldd; otool -L lists the same install names.
if (APPLE)
  set(lister_command otool -L)
else ()
  set(lister_command ldd)
endif ()
list(GET lister_command 0 lister_name)

execute_process(COMMAND ${lister_command} "${BINARY}"
    OUTPUT_VARIABLE ldd_output
    ERROR_VARIABLE ldd_error
    RESULT_VARIABLE ldd_result)

if (NOT ldd_result EQUAL 0)
  message(FATAL_ERROR
      "check_ldd.cmake: ${lister_name} failed on ${BINARY}: ${ldd_error}")
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

message(STATUS "M8-A3: dependency closure clean (no iceoryx2/zenoh), per ${lister_name}:")
message(STATUS "${ldd_output}")
