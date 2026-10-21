add_executable(${PROJECT_NAME}
    main.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE fmt::fmt spdlog::spdlog)

# Include asset copying utility
include(${CMAKE_SOURCE_DIR}/cmake/utils/CopyAssets.cmake)

# Copy assets to build directory
copy_assets_to_build_dir(
    TARGET_NAME ${PROJECT_NAME}
    ASSETS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/assets"
    ASSETS_DEST_SUBDIR "assets"
)