cmake_minimum_required(VERSION 3.31.10)

project(audio-ai LANGUAGES CXX)

# append path for local cmake modules (for find_package() to work)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

# include cmake Macros/functions/shared configs/utils
include(cmake/config/ProjectDefaults.cmake)

#include modules
include(ClangFormat)
option(BUILD_TESTING "Build the testing tree." ON)
if(BUILD_TESTING)
  enable_testing()
endif()

# dependencies
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(portaudio CONFIG REQUIRED)
find_package(Slint CONFIG REQUIRED)

get_target_property(slint_enabled_features Slint::Slint SLINT_ENABLED_FEATURES)
message(STATUS "Slint enabled features: ${slint_enabled_features}")
# subdirectories
add_subdirectory(src/modules/common)

# find_package(dth_common REQUIRED)
add_subdirectory(src)
