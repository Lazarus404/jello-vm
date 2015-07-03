cmake_minimum_required(VERSION 3.2.2)
project(jello_tests NONE)

enable_testing()

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
endif()
