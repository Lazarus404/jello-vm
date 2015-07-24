cmake_minimum_required(VERSION 3.2.2)
project(jello_std_jdll LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(jello_std_jdll MODULE std_jdll.c)
target_include_directories(jello_std_jdll PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../vm/src/include")
set_target_properties(jello_std_jdll PROPERTIES
  OUTPUT_NAME "std"
  PREFIX ""
  SUFFIX ".jdll")

if(APPLE)
  set_target_properties(jello_std_jdll PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(jello_std_jdll PRIVATE -Wall -Wextra)
endif()

install(TARGETS jello_std_jdll LIBRARY DESTINATION lib/jdll)
