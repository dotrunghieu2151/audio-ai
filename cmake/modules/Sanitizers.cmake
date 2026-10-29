
include_guard(GLOBAL)

option(DTH_ENABLE_SANITIZERS
    "Enable runtime sanitizers"
    OFF
)

set(DTH_SANITIZERS "" CACHE STRING
    "Sanitizers to enable: address, undefined, or address;undefined"
)

if(NOT DTH_ENABLE_SANITIZERS)
  return()
endif()

if(NOT DTH_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(DTH_SANITIZERS "address")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    set(DTH_SANITIZERS "address;undefined")
  else()
    message(FATAL_ERROR
            "No default sanitizer configuration for "
            "${CMAKE_CXX_COMPILER_ID}"
        )
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  set(_dth_supported_sanitizers
        address
        undefined
        thread
        leak
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(_dth_supported_sanitizers
        address
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND MSVC)
  # clang-cl requires its own platform-specific validation.
  set(_dth_supported_sanitizers address)
else()
  message(FATAL_ERROR
        "Sanitizers are not configured for "
        "${CMAKE_CXX_COMPILER_ID}"
    )
endif()

foreach(_sanitizer IN LISTS DTH_SANITIZERS)
  if(NOT _sanitizer IN_LIST _dth_supported_sanitizers)
    message(FATAL_ERROR
            "Sanitizer '${_sanitizer}' is not supported by "
            "${CMAKE_CXX_COMPILER_ID} in this module."
        )
  endif()
endforeach()

if("thread" IN_LIST DTH_SANITIZERS
    AND ("address" IN_LIST DTH_SANITIZERS
        OR "leak" IN_LIST DTH_SANITIZERS
        OR "undefined" IN_LIST DTH_SANITIZERS))
  message(FATAL_ERROR
        "ThreadSanitizer must be used in a separate build."
    )
endif()

if("address" IN_LIST DTH_SANITIZERS
    AND "leak" IN_LIST DTH_SANITIZERS)
  # Leak detection is generally integrated with ASan on Linux.
  list(REMOVE_ITEM DTH_SANITIZERS leak)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  list(JOIN DTH_SANITIZERS "," _dth_sanitizer_list)

  set(_dth_compile_flags
        "-fsanitize=${_dth_sanitizer_list}"
        "-fno-omit-frame-pointer"
    )

  set(_dth_link_flags
        "-fsanitize=${_dth_sanitizer_list}"
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(_dth_compile_flags "/fsanitize=address")
  set(_dth_link_flags "")
endif()

add_library(dth_sanitizers INTERFACE)

add_library(DTH::sanitizers ALIAS dth_sanitizers)

target_compile_options(dth_sanitizers INTERFACE
    $<$<CONFIG:Debug>:${_dth_compile_flags}>
)

target_link_options(dth_sanitizers INTERFACE
    $<$<CONFIG:Debug>:${_dth_link_flags}>
)

message(STATUS
    "Runtime sanitizers enabled: ${DTH_SANITIZERS}"
)

unset(_dth_supported_sanitizers)
unset(_dth_compile_flags)
unset(_dth_link_flags)
unset(_dth_sanitizer_list)
unset(_sanitizer)

