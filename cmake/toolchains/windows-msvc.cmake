set(CMAKE_SYSTEM_NAME Windows)

if (MINGW)
    # message(STATUS "Using MinGW on Windows")
    # set(BASE_FLAGS "-Wall -Wextra -Wpedantic -Wshadow -Wconversion -fPIC")
    # set(CMAKE_CXX_FLAGS_INIT "${BASE_FLAGS}")
    # set(CMAKE_CXX_FLAGS_DEBUG_INIT "-O0 -g3 -DDEBUG")
    # set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O3 -DNDEBUG -march=native -mtune=native")
    # set(CMAKE_CXX_FLAGS_FAST_INIT "-Ofast -DNDEBUG -march=native -mtune=native -funroll-loops -ffast-math")

    # if (ENABLE_LINTING)
    #     include(${CMAKE_SOURCE_DIR}/cmake/config/ClangTidyLint.cmake)
    #     include(${CMAKE_SOURCE_DIR}/cmake/config/CppCheckLint.cmake)
    # endif()
    message(ERROR "Using MinGW on Windows is not supported")
    return()
else()
    message(STATUS "Using MSVC compiler")
    set(CMAKE_GENERATOR_PLATFORM "x64" CACHE STRING "Target architecture")

    # # Modern C++ & strict warnings
    set(BASE_FLAGS "/W4 /permissive- /EHsc /Zc:__cplusplus /utf-8")
    set(CMAKE_CXX_FLAGS_INIT "${BASE_FLAGS}")

    set(CMAKE_CXX_FLAGS_DEBUG_INIT "/Od /Zi /MTd /DDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE_INIT "/O2 /DNDEBUG /GL /Gw")
    set(CMAKE_CXX_FLAGS_FAST_INIT "/Ox /Ot /Oi /GL /Gw /fp:fast /DNDEBUG")

    # MSVC has its built in static analysis, so we can use it as linting by adding /analyze
    if (ENABLE_LINTING)
        set(CMAKE_CXX_FLAGS_DEBUG_INIT "${CMAKE_CXX_FLAGS_DEBUG_INIT} /analyze")
        set(CMAKE_CXX_FLAGS_RELEASE_INIT "${CMAKE_CXX_FLAGS_RELEASE_INIT} /analyze")
        set(CMAKE_CXX_FLAGS_FAST_INIT "${CMAKE_CXX_FLAGS_FAST_INIT} /analyze")
    endif()
    
    # Enable Link Time Code Generation
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_FAST ON)

    # Static runtime (good for standalone apps)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

endif()

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
