cmake_minimum_required(VERSION 3.31.10)

project(
    dth_audio
    VERSION 1.0.0
    DESCRIPTION
        "Audio lib"
    LANGUAGES CXX
)
# include the GNUInstallDirs module to get the canonical install paths defined
include(GNUInstallDirs)
# if using limited visibility, set CXX_VISIBILILTY_PRESET to "hidden"
include(GenerateExportHeader)
# configure the CMake package file so the libray can be included with find_package() later
include(CMakePackageConfigHelpers)

# set the postfix "d" for the resulting .so or .dll files when building the
# library in debug mode
set(CMAKE_DEBUG_POSTFIX
    d
)

# add the library target and an alias
add_library(${PROJECT_NAME})
add_library(DTH::audio ALIAS ${PROJECT_NAME})

# set properties for the target. VERSION set the library version to the project
# version * SOVERSION set the compatibility  version for the library to the
# major number of the version
set_target_properties(
    ${PROJECT_NAME}
    PROPERTIES VERSION ${PROJECT_VERSION}
               SOVERSION ${PROJECT_VERSION_MAJOR}
               EXPORT_NAME audio
)
# add sources to the library target
target_sources(
    ${PROJECT_NAME}
    PRIVATE
      src/audio_system.cpp
      src/audio_stream.cpp
      src/pipeline/format_converter.cpp
      src/pipeline/drift_controller.cpp
      src/pipeline/device_output_sink.cpp
      src/pipeline/device_input_source.cpp
      src/pipeline/converting_sink.cpp
      src/pipeline/pipeline_stream.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE DTH::common)

# speexdsp ships no CMake package config (pkg-config only).
include(SelectLibraryConfigurations)
find_path(SPEEXDSP_INCLUDE_DIR speex/speex_resampler.h REQUIRED)
find_library(SPEEXDSP_LIBRARY_RELEASE NAMES speexdsp REQUIRED
  PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(SPEEXDSP_LIBRARY_DEBUG NAMES speexdsp
  PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
select_library_configurations(SPEEXDSP)
target_include_directories(${PROJECT_NAME} PRIVATE ${SPEEXDSP_INCLUDE_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE ${SPEEXDSP_LIBRARIES})

# System audio capture: Core Audio process taps (Objective-C++) on macOS, stub elsewhere.
if (APPLE)
  enable_language(OBJCXX)
  target_sources(${PROJECT_NAME} PRIVATE src/system_audio/macos/system_audio_source_macos.mm)
  set_source_files_properties(src/system_audio/macos/system_audio_source_macos.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion")
  # Preset CXX flags / visibility do not apply to OBJCXX.
  set_property(TARGET ${PROJECT_NAME} PROPERTY OBJCXX_VISIBILITY_PRESET "hidden")
  target_link_libraries(${PROJECT_NAME} PRIVATE "-framework CoreAudio" "-framework Foundation")
else()
  target_sources(${PROJECT_NAME} PRIVATE src/system_audio/unsupported/system_audio_source_unsupported.cpp)
endif()

add_subdirectory(src/backends/portaudio)
add_subdirectory(src/backends/cubeb)

# define the C++ standard needed to compile this library and make it visible to
# dependers
target_compile_features(
    ${PROJECT_NAME}
    PUBLIC cxx_std_23
)

# set the include directories
target_include_directories(
    ${PROJECT_NAME}
    PRIVATE src
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if (DTH_ENABLE_SANITIZERS)
  # This keeps the dependency available in the current build
  # while excluding it from both build-tree and install-tree exports.
  target_link_libraries(${PROJECT_NAME}
   PUBLIC "$<BUILD_LOCAL_INTERFACE:DTH::sanitizers>"
  )
endif()

set_property(
    TARGET ${PROJECT_NAME}
    PROPERTY CXX_VISIBILITY_PRESET "hidden"
)
# Hide inlined functions by default, reducing the size of the library
set_property(
    TARGET ${PROJECT_NAME}
    PROPERTY VISIBILITY_INLINES_HIDDEN TRUE
)

# this command generates a header file in the CMAKE_CURRENT_BINARY_DIR which
# sets the visibility attributes according to the compiler settings
generate_export_header(
    ${PROJECT_NAME}
    EXPORT_FILE_NAME
    export/${PROJECT_NAME}/export_${PROJECT_NAME}.hpp
)
# Add CMAKE_CURRENT_BINARY_DIR to the include path so the generated header can
# be found
target_include_directories(
    ${PROJECT_NAME}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/export>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)


# Install the library and export the CMake targets
install(
    TARGETS ${PROJECT_NAME}
    EXPORT  DTHAudioTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
# install the public headers
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# install the generated export header
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/export/${PROJECT_NAME}/export_${PROJECT_NAME}.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/export/${PROJECT_NAME}
)

write_basic_package_version_file(
    "${PROJECT_NAME}ConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/${PROJECT_NAME}Config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)

# install the CMake targets
install(
    EXPORT DTHAudioTargets
    FILE ${PROJECT_NAME}Targets.cmake
    NAMESPACE DTH::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

option(
    DTH_AUDIO_BUILD_TESTING
    "Build DTH_AUDIO tests"
    ${BUILD_TESTING}
)
if (DTH_AUDIO_BUILD_TESTING)
  add_subdirectory(tests)
endif()

