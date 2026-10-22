cmake_minimum_required(VERSION 3.31.10)

project(audio-ai LANGUAGES CXX)

# append path for local cmake modules (for find_package() to work)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

# include cmake Macros/functions/shared configs/utils
include(cmake/config/ProjectDefaults.cmake)

#include modules
include(ClangFormat)

# dependencies
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(portaudio CONFIG REQUIRED)

# subdirectories
add_subdirectory(src)
