cmake_minimum_required(VERSION 3.5)
project(jello_vm VERSION 0.1.0 LANGUAGES C ASM)

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
  src/interp/ops_array.c
  src/interp/ops_list.c
  src/interp/ops_enum.c
  src/interp/ops_global.c
  src/interp/ops_exc.c
  src/value/value.c
  src/gc/gc.c
  src/gc/copy.c
  src/gc/object.c
  src/proc/image.c
  src/proc/natives.c
  src/proc/net.c
  src/proc/poll.c
  src/proc/process.c
  src/proc/stack.c
  src/proc/switch.S
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
    src/jit/survive.c
    src/jit/module.c
    src/jit/reload.c
  )
  set(JELLO_JIT_TARGET_ARCH "")
  if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(JELLO_JIT_TARGET_ARCH "${CMAKE_OSX_ARCHITECTURES}")
  endif()
  if(NOT JELLO_JIT_TARGET_ARCH AND CMAKE_C_COMPILER_TARGET MATCHES "x86_64")
    set(JELLO_JIT_TARGET_ARCH "x86_64")
  elseif(NOT JELLO_JIT_TARGET_ARCH AND CMAKE_C_COMPILER_TARGET MATCHES "aarch64|arm64")
    set(JELLO_JIT_TARGET_ARCH "aarch64")
  endif()
  if(NOT JELLO_JIT_TARGET_ARCH)
    set(JELLO_JIT_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  if(JELLO_JIT_TARGET_ARCH MATCHES "^(x86_64|AMD64)$")
    target_sources(jello_vm PRIVATE src/jit/emit_x64.c src/jit/regalloc_x64.c)
  elseif(JELLO_JIT_TARGET_ARCH MATCHES "^(aarch64|arm64|ARM64)$")
    target_sources(jello_vm PRIVATE src/jit/emit_arm64.c src/jit/regalloc_arm64.c)
  else()
    message(FATAL_ERROR "JELLO_ENABLE_JIT: unsupported CMAKE target arch '${JELLO_JIT_TARGET_ARCH}'")
  endif()
  target_compile_definitions(jello_vm PRIVATE JELLO_ENABLE_JIT)
else()
  target_sources(jello_vm PRIVATE src/jit/none.c)
endif()
if(CMAKE_C_COMPILER_TARGET)
  set_property(SOURCE src/proc/switch.S tools/jelloc_image.S APPEND_STRING PROPERTY
    COMPILE_FLAGS " --target=${CMAKE_C_COMPILER_TARGET}")
endif()
set(JELLO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${JELLO_GENERATED_DIR}/jello_version.h.tmp" "#define JELLO_VERSION \"${PROJECT_VERSION}\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${JELLO_GENERATED_DIR}/jello_version.h.tmp" "${JELLO_GENERATED_DIR}/jello_version.h")
target_include_directories(jello_vm PUBLIC src/include)
if(WIN32)
  target_sources(jello_vm PRIVATE third_party/wepoll-1.5.8/wepoll.c)
  set_source_files_properties(third_party/wepoll-1.5.8/wepoll.c PROPERTIES COMPILE_OPTIONS "-w")
endif()
target_compile_definitions(jello_vm PRIVATE JELLO_JDLL_HOST)
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

add_library(jello_host STATIC tools/host.c tools/run.c tools/boot.c tools/deps.c)
target_include_directories(jello_host PUBLIC tools "${JELLO_GENERATED_DIR}")
target_link_libraries(jello_host PUBLIC jello_vm)
jello_enable_warnings(jello_host)
if(JELLO_WARNINGS_AS_ERRORS)
  target_compile_options(jello_host PRIVATE -Werror)
endif()

add_executable(jlo_tool tools/jlo_tool.c)
target_link_libraries(jlo_tool PRIVATE jello_host)
jello_enable_warnings(jlo_tool)
if(JELLO_WARNINGS_AS_ERRORS)
  target_compile_options(jlo_tool PRIVATE -Werror)
endif()

if(WIN32)
  add_executable(jello tools/jello.c tools/manifest.rc)
else()
  add_executable(jello tools/jello.c)
endif()
target_link_libraries(jello PRIVATE jello_host)
jello_enable_warnings(jello)
if(JELLO_WARNINGS_AS_ERRORS)
  target_compile_options(jello PRIVATE -Werror)
endif()

set(JELLOC_IMAGE "" CACHE FILEPATH "The compiler module jelloc embeds")
if(JELLOC_IMAGE AND EXISTS "${JELLOC_IMAGE}")
  add_executable(jelloc tools/jelloc.c tools/jelloc_image.S)
  set_source_files_properties(tools/jelloc_image.S PROPERTIES
    COMPILE_DEFINITIONS "JELLOC_IMAGE=\"${JELLOC_IMAGE}\""
    OBJECT_DEPENDS "${JELLOC_IMAGE}")
  target_link_libraries(jelloc PRIVATE jello_host)
  jello_enable_warnings(jelloc)
  if(JELLO_WARNINGS_AS_ERRORS)
    target_compile_options(jelloc PRIVATE -Werror)
  endif()
endif()

function(jello_jdll target name)
  add_library(${target} MODULE ${ARGN})
  target_include_directories(${target} PRIVATE src/include "${JELLO_GENERATED_DIR}")
  set_target_properties(${target} PROPERTIES OUTPUT_NAME "${name}" PREFIX "" SUFFIX ".jdll")
  if(APPLE)
    set_target_properties(${target} PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
  endif()
  jello_enable_warnings(${target})
  if(JELLO_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
  install(TARGETS ${target} LIBRARY DESTINATION lib/jdll)
endfunction()

set(JELLO_STD_SOURCES
  std/std.c
  std/date.c
  std/encode.c
  std/float.c
  std/io.c
  std/math.c
  std/object.c
  std/random.c
  std/regexp.c
  std/text.c
  std/unicode.c)
if(WIN32)
  list(APPEND JELLO_STD_SOURCES std/command_windows.c)
else()
  list(APPEND JELLO_STD_SOURCES std/command.c)
endif()
add_library(jello_pcre2 STATIC
  third_party/pcre2-10.30/src/pcre2_auto_possess.c
  third_party/pcre2-10.30/src/pcre2_chartables.c
  third_party/pcre2-10.30/src/pcre2_compile.c
  third_party/pcre2-10.30/src/pcre2_config.c
  third_party/pcre2-10.30/src/pcre2_context.c
  third_party/pcre2-10.30/src/pcre2_convert.c
  third_party/pcre2-10.30/src/pcre2_dfa_match.c
  third_party/pcre2-10.30/src/pcre2_error.c
  third_party/pcre2-10.30/src/pcre2_find_bracket.c
  third_party/pcre2-10.30/src/pcre2_jit_compile.c
  third_party/pcre2-10.30/src/pcre2_maketables.c
  third_party/pcre2-10.30/src/pcre2_match.c
  third_party/pcre2-10.30/src/pcre2_match_data.c
  third_party/pcre2-10.30/src/pcre2_newline.c
  third_party/pcre2-10.30/src/pcre2_ord2utf.c
  third_party/pcre2-10.30/src/pcre2_pattern_info.c
  third_party/pcre2-10.30/src/pcre2_serialize.c
  third_party/pcre2-10.30/src/pcre2_string_utils.c
  third_party/pcre2-10.30/src/pcre2_study.c
  third_party/pcre2-10.30/src/pcre2_substitute.c
  third_party/pcre2-10.30/src/pcre2_substring.c
  third_party/pcre2-10.30/src/pcre2_tables.c
  third_party/pcre2-10.30/src/pcre2_ucd.c
  third_party/pcre2-10.30/src/pcre2_valid_utf.c
  third_party/pcre2-10.30/src/pcre2_xclass.c)
target_include_directories(jello_pcre2 PRIVATE third_party/pcre2-10.30/src)
target_compile_definitions(jello_pcre2 PRIVATE HAVE_CONFIG_H PCRE2_CODE_UNIT_WIDTH=8 PCRE2_STATIC
  SUPPORT_UNICODE HAVE_STDINT_H=1 HAVE_INTTYPES_H=1 HAVE_MEMMOVE=1 HAVE_STRERROR=1)
set_target_properties(jello_pcre2 PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
jello_jdll(jello_std_jdll std ${JELLO_STD_SOURCES})
target_include_directories(jello_std_jdll PRIVATE third_party/pcre2-10.30/src)
target_link_libraries(jello_std_jdll PRIVATE jello_pcre2)

file(GLOB JELLO_MBEDTLS_SOURCES "${CMAKE_CURRENT_LIST_DIR}/third_party/mbedtls-3.5.1/library/*.c")
add_library(jello_mbedtls STATIC ${JELLO_MBEDTLS_SOURCES})
target_include_directories(jello_mbedtls SYSTEM PUBLIC third_party/mbedtls-3.5.1/include)
target_include_directories(jello_mbedtls PRIVATE src/include)
target_compile_definitions(jello_mbedtls PUBLIC "MBEDTLS_USER_CONFIG_FILE=<jello/internal/tls_config.h>")
set_target_properties(jello_mbedtls PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_link_libraries(jello_mbedtls PUBLIC Threads::Threads)
jello_jdll(jello_tls_jdll tls tls/tls.c tls/crypto.c)
target_link_libraries(jello_tls_jdll PRIVATE jello_mbedtls)

enable_testing()
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../tests" tests)
