cmake_minimum_required(VERSION 3.2.2)
project(jello_tests NONE)

enable_testing()

add_test(NAME build_matrix
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/build_matrix.sh"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
