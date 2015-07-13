cmake_minimum_required(VERSION 3.2.2)
project(jello_vm LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(CompilerWarnings)

option(JELLO_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(JELLO_USE_COMPUTED_GOTO "Use computed-goto dispatch on GCC/Clang (Chapter 4)" ON)

add_library(jello_vm STATIC
  src/load/loader.c
  src/load/check.c
  src/interp/frame.c
  src/interp/dispatch.c
  src/interp/exec_loop.c
  src/interp/ops_arith.c
  src/interp/ops_bytes.c
  src/interp/ops_dyn.c
  src/interp/ops_struct.c
  src/value/value.c
  src/gc/gc.c
)
target_include_directories(jello_vm PUBLIC src/include)
if(JELLO_USE_COMPUTED_GOTO)
  target_compile_definitions(jello_vm PRIVATE JELLO_USE_COMPUTED_GOTO)
endif()
jello_enable_warnings(jello_vm)
if(JELLO_WARNINGS_AS_ERRORS)
  target_compile_options(jello_vm PRIVATE -Werror)
endif()

install(FILES src/include/jello.h DESTINATION include)
install(FILES src/include/jello/jdll.h DESTINATION include/jello)

add_executable(jlo_tool tools/jlo_tool.c)
target_include_directories(jlo_tool PRIVATE src/include)
target_link_libraries(jlo_tool PRIVATE jello_vm)
jello_enable_warnings(jlo_tool)
if(JELLO_WARNINGS_AS_ERRORS)
  target_compile_options(jlo_tool PRIVATE -Werror)
endif()

enable_testing()
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../tests" tests)
