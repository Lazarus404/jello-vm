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

  add_test(NAME chapter5_gc_v1
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/gc_fixtures.sh" "$<TARGET_FILE:jlo_tool>"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_test(NAME chapter6_dynamic_and_objects
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/dyn_fixtures.sh" "$<TARGET_FILE:jlo_tool>"
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
  TIMEOUT 60)
