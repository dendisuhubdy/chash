# These two wrappers are used to gather in-depth information about the
# hashing process. Using them does not yield proper timing information
# about the overall compilation process.
configure_file(clang-hash.in ${PROJECT_BINARY_DIR}/wrappers/clang-hash)
configure_file(clang-hash-collect.in ${PROJECT_BINARY_DIR}/wrappers/clang-hash-collect)

# The same for gcc
configure_file(gcc-hash.in ${PROJECT_BINARY_DIR}/wrappers/gcc-hash)


# These wrappers can be used for the actual (re)compilation of whole procects.
# - clang-normal: normal clang operation; no speedup
# - clang-ccache: ccache assisted clang
# - clang-hash-stop: clang-hash assisted clang

find_program(CCACHE NAMES "ccache")
if (NOT CCACHE)
  message(ERROR "CCACHE not found")
endif()
configure_file(clang-normal.in ${PROJECT_BINARY_DIR}/wrappers/clang-normal)
configure_file(clang-ccache.in ${PROJECT_BINARY_DIR}/wrappers/clang-ccache)
configure_file(clang-hash-stop.in ${PROJECT_BINARY_DIR}/wrappers/clang-hash-stop)
configure_file(clang-ccache-hash-stop.in ${PROJECT_BINARY_DIR}/wrappers/clang-ccache-hash-stop)

# Symlink compiler wrappers
execute_process(
  COMMAND "${CMAKE_COMMAND}" "-E" "create_symlink"
    "${CMAKE_CURRENT_SOURCE_DIR}/gcc" "${CMAKE_CURRENT_BINARY_DIR}/gcc"
)

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-E" "create_symlink"
    "${CMAKE_CURRENT_SOURCE_DIR}/compare-logs" "${CMAKE_CURRENT_BINARY_DIR}/compare-logs"
)

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-E" "create_symlink"
    "${CMAKE_CURRENT_SOURCE_DIR}/hash-objectfile" "${CMAKE_CURRENT_BINARY_DIR}/hash-objectfile"
)

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-E" "create_symlink"
    "${CMAKE_CURRENT_SOURCE_DIR}/gcc-time" "${CMAKE_CURRENT_BINARY_DIR}/gcc-time"
)
