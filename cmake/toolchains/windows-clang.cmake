set(CMAKE_SYSTEM_NAME Windows)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)

# Use LLVM linker (optional but recommended)
set(CMAKE_LINKER lld-link)

# clang-cl still uses MSVC runtime model
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------
# Compiler flags
# ---------------------------------
set(BASE_FLAGS "-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-unused-parameter")

set(CMAKE_CXX_FLAGS_INIT "${BASE_FLAGS}")
set(CMAKE_CXX_FLAGS_DEBUG_INIT "-O0 -g3 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O3 -DNDEBUG -march=native")
set(CMAKE_CXX_FLAGS_FAST_INIT "-Ofast -DNDEBUG -march=native -funroll-loops -ffast-math")

# Enable LTO in Release/Fast
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_FAST ON)

# vcpkg integration
if (NOT VCPKG_ROOT)
    # if VCPKG_ROOT is not set, use the local vcpkg.cmake
    include(${CMAKE_CURRENT_LIST_DIR}/vcpkg.cmake)
    message(STATUS "Using local vcpkg.cmake")
else()
    # if VCPKG_ROOT is set, use the prebuilt vcpkg
    include("${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    message(STATUS "Using prebuilt vcpkg.cmake")
endif()

if (ENABLE_LINTING)
    include(${CMAKE_SOURCE_DIR}/cmake/config/IncludeHeaderCheck.cmake)
endif()
