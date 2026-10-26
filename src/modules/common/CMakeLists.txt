cmake_minimum_required(VERSION 3.31.10)

project(
    dth_common
    VERSION 1.0.0
    DESCRIPTION
        "Shared common utils"
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
add_library(DTH::common ALIAS ${PROJECT_NAME})

# set properties for the target. VERSION set the library version to the project
# version * SOVERSION set the compatibility  version for the library to the
# major number of the version
set_target_properties(
    ${PROJECT_NAME}
    PROPERTIES VERSION ${PROJECT_VERSION}
               SOVERSION ${PROJECT_VERSION_MAJOR}
               EXPORT_NAME common
)
# add sources to the library target
target_sources(
    ${PROJECT_NAME}
    PRIVATE src/timer.cpp
)

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
    EXPORT DTHCommonTargets
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
    EXPORT DTHCommonTargets
    FILE ${PROJECT_NAME}Targets.cmake
    NAMESPACE DTH::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)
