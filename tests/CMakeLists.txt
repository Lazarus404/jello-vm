cmake_minimum_required(VERSION 3.2.2)
project(jello_tests NONE)

enable_testing()

set(JLO_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/../build/jlo_tool" CACHE FILEPATH "Path to a built jlo_tool executable")

add_test(NAME build_matrix
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/build_matrix.sh"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_test(NAME bytecode_fixtures
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/bytecode_fixtures.sh" "${JLO_TOOL}"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
