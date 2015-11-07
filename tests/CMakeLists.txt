add_test(NAME build_matrix
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/build_matrix.sh"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

if(TARGET jlo_tool)
  add_test(NAME chapter3_bytecode_and_validation
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/bytecode_fixtures.sh" "$<TARGET_FILE:jlo_tool>"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_test(NAME chapter4_interpreter_core
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/interp_fixtures.sh" "$<TARGET_FILE:jlo_tool>"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_test(NAME chapter5_gc
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/gc_fixtures.sh" "$<TARGET_FILE:jlo_tool>"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_test(NAME chapter6_dynamic_and_objects
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/dyn_fixtures.sh" "$<TARGET_FILE:jlo_tool>"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_test(NAME chapter8_bytes_layout
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/packet_fixtures.sh" "$<TARGET_FILE:jlo_tool>"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_test(NAME chapter9_jir_lowering
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/jir_fixtures.sh" "$<TARGET_FILE:jlo_tool>"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

add_library(test_native_jdll MODULE jdll/test_native.c)
target_include_directories(test_native_jdll PRIVATE "${CMAKE_SOURCE_DIR}/src/include")
set_target_properties(test_native_jdll PROPERTIES
  OUTPUT_NAME "test_native"
  PREFIX ""
  SUFFIX ".jdll"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/jdll")
jello_enable_warnings(test_native_jdll)
if(JELLO_WARNINGS_AS_ERRORS)
  target_compile_options(test_native_jdll PRIVATE -Werror)
endif()
if(APPLE)
  set_target_properties(test_native_jdll PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
endif()

set(JELLO_JDLL_ASAN_OPTIONS "ASAN_OPTIONS=detect_odr_violation=0")

if(TARGET jlo_tool)
  add_test(NAME chapter7_jdll_and_std
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/jdll_fixtures.sh" "$<TARGET_FILE:jlo_tool>"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(chapter7_jdll_and_std PROPERTIES
    ENVIRONMENT "JELLO_JDLL_PATH=$<TARGET_FILE_DIR:test_native_jdll>;${JELLO_JDLL_ASAN_OPTIONS}")
endif()

add_executable(chapter7_thread_tests csrc/chapter7_thread_tests.c)
target_link_libraries(chapter7_thread_tests PRIVATE jello_vm)
jello_enable_warnings(chapter7_thread_tests)
if(JELLO_WARNINGS_AS_ERRORS)
  target_compile_options(chapter7_thread_tests PRIVATE -Werror)
endif()

add_test(NAME chapter7_threads_and_stop_the_world
  COMMAND chapter7_thread_tests
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(chapter7_threads_and_stop_the_world PROPERTIES
  ENVIRONMENT "JELLO_JDLL_PATH=$<TARGET_FILE_DIR:test_native_jdll>:$<TARGET_FILE_DIR:jello_std_jdll>;${JELLO_JDLL_ASAN_OPTIONS}"
  TIMEOUT 60)

add_executable(chapter9_jir_tests csrc/chapter9_jir_tests.c)
target_link_libraries(chapter9_jir_tests PRIVATE jello_vm)
jello_enable_warnings(chapter9_jir_tests)
if(JELLO_WARNINGS_AS_ERRORS)
  target_compile_options(chapter9_jir_tests PRIVATE -Werror)
endif()

add_test(NAME chapter9_jir_verify
  COMMAND chapter9_jir_tests
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

if(JELLO_ENABLE_JIT)
  add_executable(jit_frame_abi_tests csrc/jit_frame_abi_tests.c)
  target_link_libraries(jit_frame_abi_tests PRIVATE jello_vm)
  jello_enable_warnings(jit_frame_abi_tests)
  if(JELLO_WARNINGS_AS_ERRORS)
    target_compile_options(jit_frame_abi_tests PRIVATE -Werror)
  endif()

  add_test(NAME chapter10_jit_frame_abi
    COMMAND jit_frame_abi_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  add_test(NAME chapter10_jit_frame_abi_x64
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/jit_frame_abi_x64.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
