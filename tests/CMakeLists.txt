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
