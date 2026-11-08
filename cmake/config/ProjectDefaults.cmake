# set standard flags, C++ version, warnings
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Add custom configuration type: Fast
if(NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Fast" CACHE STRING "Build configs" FORCE)
else()
  list(APPEND CMAKE_CONFIGURATION_TYPES Fast)
endif()

# Imported targets (vcpkg) only ship Debug/Release; map Fast to Release
set(CMAKE_MAP_IMPORTED_CONFIG_FAST Release "")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)


# Set output directories with configuration
# Organize build outputs by config
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/archive)
