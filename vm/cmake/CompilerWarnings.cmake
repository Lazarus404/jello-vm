function(jello_enable_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE
      /W4
      /permissive-
    )
    if(JELLO_WARNINGS_AS_ERRORS)
      target_compile_options(${target_name} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target_name} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wstrict-prototypes
      -Wmissing-prototypes
      -Wold-style-definition
      -Wno-unused-parameter
    )
    if(JELLO_WARNINGS_AS_ERRORS)
      target_compile_options(${target_name} PRIVATE -Werror)
    endif()
  endif()
endfunction()
