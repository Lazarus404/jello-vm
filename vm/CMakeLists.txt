cmake_minimum_required(VERSION 3.2.2)
project(jello_vm LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(CompilerWarnings)

option(JELLO_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(JELLO_USE_COMPUTED_GOTO "Use computed-goto dispatch on GCC/Clang" ON)
option(JELLO_ENABLE_JIT "Compile the JIT backend into jello_vm" ON)

add_library(jello_vm STATIC
  src/load/loader.c
  src/load/check.c
  src/load/op_table.c
  src/interp/frame.c
  src/interp/dispatch.c
  src/interp/exec_loop.c
  src/interp/ops_arith.c
  src/interp/ops_bytes.c
  src/interp/ops_dyn.c
  src/interp/ops_struct.c
  src/interp/ops_obj.c
  src/value/value.c
  src/gc/gc.c
  src/gc/object.c
  src/thread/thread.c
  src/jdll/loader.c
  src/jdll/marshal.c
  src/jdll/call_native.c
  src/jit/ir.c
)
if(JELLO_ENABLE_JIT)
  target_sources(jello_vm PRIVATE
    src/jit/emit_common.c
    src/jit/frame.c
    src/jit/regalloc.c
    src/jit/emit_x64.c
    src/jit/module.c
  )
  target_compile_definitions(jello_vm PRIVATE JELLO_ENABLE_JIT)
endif()
target_include_directories(jello_vm PUBLIC src/include)
if(JELLO_USE_COMPUTED_GOTO)
  target_compile_definitions(jello_vm PRIVATE JELLO_USE_COMPUTED_GOTO)
endif()
jello_enable_warnings(jello_vm)
if(JELLO_WARNINGS_AS_ERRORS)
  target_compile_options(jello_vm PRIVATE -Werror)
endif()
find_package(Threads REQUIRED)
target_link_libraries(jello_vm PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

if(APPLE)
  target_link_libraries(jello_vm INTERFACE "-Wl,-force_load,$<TARGET_FILE:jello_vm>")
else()
  target_link_libraries(jello_vm INTERFACE "-Wl,--whole-archive" "$<TARGET_FILE:jello_vm>" "-Wl,--no-whole-archive")
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

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../jdll-std" jdll-std)

enable_testing()
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../tests" tests)
