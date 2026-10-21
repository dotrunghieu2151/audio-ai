set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
# Build for native architecture only (arm64 on Apple Silicon, x86_64 on Intel)
# This ensures vcpkg packages match the build architecture
# Note: On Apple Silicon, this should be "arm64". On Intel Macs, use "x86_64"

# set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build for both Apple Silicon and Intel")

set(BASE_FLAGS "-Wall -Wextra -Wpedantic -Wshadow -Wconversion -fvisibility=hidden -fPIC")

set(CMAKE_CXX_FLAGS_INIT "${BASE_FLAGS}")
set(CMAKE_CXX_FLAGS_DEBUG_INIT "-O0 -g3 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O3 -DNDEBUG -march=native -mtune=native")
set(CMAKE_CXX_FLAGS_FAST_INIT "-Ofast -DNDEBUG -march=native -mtune=native -funroll-loops -ffast-math")

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_FAST ON)

if (NOT DEFINED VCPKG_ROOT)
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
    include(${CMAKE_SOURCE_DIR}/cmake/config/ClangTidyLint.cmake)
    include(${CMAKE_SOURCE_DIR}/cmake/config/CppCheckLint.cmake)
endif()
