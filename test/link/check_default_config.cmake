# test/link/check_default_config.cmake — M8-A1: the default configuration is
# lib-only.
#
# Runs a FRESH CMake configure of this source tree with NO options passed and
# asserts the backend options default to OFF. Grepping the current build's
# cache would be tautological (this build may have been configured with
# explicit options); only a fresh default configure proves the DEFAULTS.
#
# Usage: cmake -DSOURCE_DIR=<repo> -DPROBE_DIR=<scratch build dir> -P check_default_config.cmake

if (NOT DEFINED SOURCE_DIR OR NOT DEFINED PROBE_DIR)
  message(FATAL_ERROR "check_default_config.cmake: pass -DSOURCE_DIR and -DPROBE_DIR")
endif ()

file(REMOVE_RECURSE "${PROBE_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${PROBE_DIR}"
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
    RESULT_VARIABLE configure_result)
if (NOT configure_result EQUAL 0)
  message(FATAL_ERROR "default configure failed:\n${configure_output}\n${configure_error}")
endif ()

file(READ "${PROBE_DIR}/CMakeCache.txt" cache_contents)
foreach (backend ICEORYX2 ZENOH)
  if (NOT cache_contents MATCHES "XMMESSAGING_WITH_${backend}:BOOL=OFF")
    message(FATAL_ERROR
        "M8-A1 VIOLATED: XMMESSAGING_WITH_${backend} does not default to OFF "
        "in a fresh configure — backends must be strictly opt-in")
  endif ()
endforeach ()

message(STATUS "M8-A1: default configuration is lib-only "
    "(XMMESSAGING_WITH_ICEORYX2=OFF, XMMESSAGING_WITH_ZENOH=OFF)")
