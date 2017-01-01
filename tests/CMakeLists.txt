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

  add_test(NAME chapter18_fuel_and_limits
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/fuel_fixtures.sh" "$<TARGET_FILE:jlo_tool>"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

add_executable(trap_pc_tests csrc/trap_pc_tests.c)
target_link_libraries(trap_pc_tests PRIVATE jello_vm)
jello_enable_warnings(trap_pc_tests)
if(JELLO_WARNINGS_AS_ERRORS)
  target_compile_options(trap_pc_tests PRIVATE -Werror)
endif()

add_test(NAME chapter18_trap_pc
  COMMAND trap_pc_tests
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

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

  add_executable(jit_numeric_tests csrc/jit_numeric_tests.c)
  target_link_libraries(jit_numeric_tests PRIVATE jello_vm)
  jello_enable_warnings(jit_numeric_tests)
  if(JELLO_WARNINGS_AS_ERRORS)
    target_compile_options(jit_numeric_tests PRIVATE -Werror)
  endif()

  add_executable(jit_numeric_run csrc/jit_numeric_run.c)
  target_link_libraries(jit_numeric_run PRIVATE jello_vm)
  jello_enable_warnings(jit_numeric_run)
  if(JELLO_WARNINGS_AS_ERRORS)
    target_compile_options(jit_numeric_run PRIVATE -Werror)
  endif()

  add_executable(jit_cfg_tests csrc/jit_cfg_tests.c)
  target_link_libraries(jit_cfg_tests PRIVATE jello_vm)
  jello_enable_warnings(jit_cfg_tests)
  if(JELLO_WARNINGS_AS_ERRORS)
    target_compile_options(jit_cfg_tests PRIVATE -Werror)
  endif()

  add_executable(jit_calls_tests csrc/jit_calls_tests.c)
  target_link_libraries(jit_calls_tests PRIVATE jello_vm)
  jello_enable_warnings(jit_calls_tests)
  if(JELLO_WARNINGS_AS_ERRORS)
    target_compile_options(jit_calls_tests PRIVATE -Werror)
  endif()

  add_test(NAME chapter9d_jit_calls
    COMMAND jit_calls_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  add_test(NAME chapter9d_jit_calls_x64
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/jit_calls_x86.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  if(TARGET jlo_tool)
    add_test(NAME chapter10_jit_conform
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/jit_numeric_conform.sh"
              "$<TARGET_FILE:jlo_tool>" "$<TARGET_FILE:jit_numeric_run>"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME chapter10_jit_conform_x64
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/jit_numeric_conform_x64.sh"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(chapter10_jit_conform_x64 PROPERTIES TIMEOUT 300)

    add_executable(jit_reload_tests csrc/jit_reload_tests.c)
    target_link_libraries(jit_reload_tests PRIVATE jello_vm)
    jello_enable_warnings(jit_reload_tests)
    if(JELLO_WARNINGS_AS_ERRORS)
      target_compile_options(jit_reload_tests PRIVATE -Werror)
    endif()

    add_test(NAME chapter16_jit_reload
      COMMAND jit_reload_tests
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

    add_test(NAME chapter16_jit_reload_x64
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/jit_reload_x64.sh"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    if(TARGET jlo_tool)
      add_test(NAME chapter16_name_atom_roundtrip
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/name_atom_roundtrip.sh" "$<TARGET_FILE:jlo_tool>"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()
  
    add_executable(chapter13_jit_arm64_tests csrc/chapter13_jit_arm64_tests.c)
    target_link_libraries(chapter13_jit_arm64_tests PRIVATE jello_vm)
    jello_enable_warnings(chapter13_jit_arm64_tests)
    if(JELLO_WARNINGS_AS_ERRORS)
      target_compile_options(chapter13_jit_arm64_tests PRIVATE -Werror)
    endif()

    add_executable(chapter13_jit_golden_tests csrc/chapter13_jit_golden_tests.c)
    target_link_libraries(chapter13_jit_golden_tests PRIVATE jello_vm)
    jello_enable_warnings(chapter13_jit_golden_tests)
    if(JELLO_WARNINGS_AS_ERRORS)
      target_compile_options(chapter13_jit_golden_tests PRIVATE -Werror)
    endif()

    add_test(NAME chapter13_jit_arm64
      COMMAND chapter13_jit_arm64_tests
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

    add_test(NAME chapter13_jit_golden
      COMMAND chapter13_jit_golden_tests
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

    if(TARGET jlo_tool)
      add_test(NAME chapter13_jit_arm64_native
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/chapter13_jit_arm64_native.sh"
                "${CMAKE_CURRENT_BINARY_DIR}" "$<TARGET_FILE:jlo_tool>"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
      set_tests_properties(chapter13_jit_arm64_native PROPERTIES TIMEOUT 120)
    endif()
  endif()

  add_test(NAME chapter11_jit_numeric
    COMMAND jit_numeric_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  add_test(NAME chapter11_jit_numeric_x64
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/jit_numeric_x64.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_test(NAME chapter12_jit_cfg
    COMMAND jit_cfg_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  add_test(NAME chapter12_jit_cfg_x64
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/ci/jit_cfg_x64.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
