# *-* Mode: cmake; *-*

cmake_minimum_required(VERSION 2.6)
project(rr)

enable_testing()
set(BUILD_SHARED_LIBS ON)

set(PROJECT_BINARY_DIR ${CMAKE_SOURCE_DIR}/obj)
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)

set(rr_VERSION_MAJOR 0)
set(rr_VERSION_MINOR 1)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g3 -Wall -m32")

# Disable PIC.
string(REGEX REPLACE "-fPIC" "-pthread"
  CMAKE_SHARED_LIBRARY_C_FLAGS "${CMAKE_SHARED_LIBRARY_C_FLAGS}")

add_library(rr_wrap_syscalls
  src/share/wrap_syscalls.c
)

add_executable(rr
  src/main.c
  src/recorder/handle_ioctl.c
  src/recorder/handle_signal.c
  src/recorder/rec_process_event.c
  src/recorder/rec_sched.c
  src/recorder/recorder.c
  src/replayer/rep_process_event.c
  src/replayer/rep_process_signal.c
  src/replayer/rep_sched.c
  src/replayer/replayer.c
  src/share/hpc.c
  src/share/ipc.c
  src/share/list.c
  src/share/shmem.c
  src/share/sys.c
  src/share/trace.c
  src/share/util.c
)

target_link_libraries(rr
  pfm
  disasm
)

# TODO: probably want to manage the tests with the build system to
# avoid unnecessary rebuilding etc.
add_test(Tests "${CMAKE_SOURCE_DIR}/src/script/test.sh" "-dir=${CMAKE_SOURCE_DIR}/src/test" "-rr=${EXECUTABLE_OUTPUT_PATH}/rr")
set_tests_properties(Tests PROPERTIES FAIL_REGULAR_EXPRESSION "FAILED")
# Why isn't dumping output on failure default ...
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --verbose)