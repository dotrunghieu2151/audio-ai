add_executable(${PROJECT_NAME}
    main.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE fmt::fmt spdlog::spdlog portaudio_static Slint::Slint)

# Include asset copying utility
include(${CMAKE_SOURCE_DIR}/cmake/utils/CopyAssets.cmake)

slint_target_sources(${PROJECT_NAME} ui/app-window.slint)
# On Windows, copy the Slint DLL next to the application binary so that it's found.
if (WIN32)
  add_custom_command(TARGET my_application POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}> $<TARGET_FILE_DIR:${PROJECT_NAME}> COMMAND_EXPAND_LISTS)
endif()


# Copy assets to build directory
copy_assets_to_build_dir(
    TARGET_NAME ${PROJECT_NAME}
    ASSETS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/assets"
    ASSETS_DEST_SUBDIR "assets"
)
