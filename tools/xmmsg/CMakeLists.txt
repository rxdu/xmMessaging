# xmmsg — the R5 external-introspection CLI (M10). Ships with the library
# (deb payload alongside it, family pattern: xmtelemetry-* tools); gated by
# XMMESSAGING_BUILD_TOOLS (default ON). Requires the POSIX shm backend on
# Linux — without it the binary still builds but reports the configuration
# and exits (never a silent no-op).
add_executable(xmmsg xmmsg.cpp)
target_link_libraries(xmmsg PRIVATE xmotion::xmmessaging)
target_compile_options(xmmsg PRIVATE ${XMMESSAGING_WARNINGS})

# Part of the shipped payload (family pattern: the telemetry CLIs install
# to bin/ next to the library).
install(TARGETS xmmsg RUNTIME DESTINATION bin)
