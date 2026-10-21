# =============================================================================
# Format.cmake
# -----------------------------------------------------------------------------
# Adds `format` and `check-format` targets using clang-format.
# Works cross-platform and supports nested source/include/test directories.
# =============================================================================

# Find clang-format executable
find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)

if(NOT CLANG_FORMAT_EXECUTABLE)
    message(WARNING "clang-format not found! 'format' and 'check-format' targets will be unavailable.")
    return()
endif()

message(STATUS "clang-format found: ${CLANG_FORMAT_EXECUTABLE}")

# -----------------------------------------------------------------------------
# Define source directories for formatting
# -----------------------------------------------------------------------------
if(NOT DEFINED PROJECT_SOURCE_DIR)
    get_filename_component(PROJECT_SOURCE_DIR "${CMAKE_SOURCE_DIR}" ABSOLUTE)
endif()

# Gather all C/C++ files recursively
file(GLOB_RECURSE FORMAT_SOURCE_FILES
    LIST_DIRECTORIES false
    CONFIGURE_DEPENDS
     ${PROJECT_SOURCE_DIR}/src/*.cpp
     ${PROJECT_SOURCE_DIR}/src/*.h
     ${PROJECT_SOURCE_DIR}/src/*.hpp
     ${PROJECT_SOURCE_DIR}/tests/*.cpp
     ${PROJECT_SOURCE_DIR}/tests/*.h
     ${PROJECT_SOURCE_DIR}/tests/*.hpp
     ${PROJECT_SOURCE_DIR}/include/*.hpp
     ${PROJECT_SOURCE_DIR}/include/*.h
)

message(STATUS "Formatting source directories: ${PROJECT_SOURCE_DIR}")
message(STATUS "Formatting source files: ${FORMAT_SOURCE_FILES}")

if(NOT FORMAT_SOURCE_FILES)
    message(WARNING "No source files found for clang-format in: ${FORMAT_SOURCE_DIRS}")
    return()
endif()

# -----------------------------------------------------------------------------
# format: automatically format source files
# -----------------------------------------------------------------------------
add_custom_target(format
    COMMAND ${CLANG_FORMAT_EXECUTABLE}
            -style=file
            -i
            ${FORMAT_SOURCE_FILES}
    WORKING_DIRECTORY
      ${CMAKE_SOURCE_DIR}
    COMMENT "Running clang-format to reformat all source files"
)

# -----------------------------------------------------------------------------
# check-format: verify formatting without changing files
# -----------------------------------------------------------------------------
add_custom_target(check-format
    COMMAND ${CLANG_FORMAT_EXECUTABLE}
            --dry-run
            --Werror
            -style=file
            -i
            ${FORMAT_SOURCE_FILES}
    WORKING_DIRECTORY
      ${CMAKE_SOURCE_DIR}
    COMMENT "Checking code format compliance (no changes made)"
)

# -----------------------------------------------------------------------------
# Print summary
# -----------------------------------------------------------------------------
message(STATUS "Added targets:
  - format        : Formats code using clang-format
  - check-format  : Checks formatting compliance
")
