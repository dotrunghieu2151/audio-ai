#[[
    copy_assets_to_build_dir

    Creates a custom target to copy assets to the build directory.
    Only copies when asset files change (tracked via stamp file).

    Parameters:
        TARGET_NAME       - Name of the target to add dependency to
        ASSETS_SOURCE_DIR - Source directory containing assets (default: ${CMAKE_SOURCE_DIR}/assets)
        ASSETS_DEST_SUBDIR - Subdirectory name in output directory (default: assets)

    Example:
        copy_assets_to_build_dir(
            TARGET_NAME myapp
            ASSETS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/resources"
            ASSETS_DEST_SUBDIR "resources"
        )
]]
function(copy_assets_to_build_dir)
    # Parse arguments
    cmake_parse_arguments(
        ARGS                                    # Prefix for parsed variables
        ""                                      # Options (boolean flags)
        "TARGET_NAME;ASSETS_SOURCE_DIR;ASSETS_DEST_SUBDIR"  # Single-value arguments
        ""                                      # Multi-value arguments
        ${ARGN}                                 # Input arguments
    )

    # Set defaults
    if(NOT DEFINED ARGS_ASSETS_SOURCE_DIR)
        set(ARGS_ASSETS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/assets")
    endif()

    if(NOT DEFINED ARGS_ASSETS_DEST_SUBDIR)
        set(ARGS_ASSETS_DEST_SUBDIR "assets")
    endif()

    # Validate required arguments
    if(NOT DEFINED ARGS_TARGET_NAME)
        message(FATAL_ERROR "copy_assets_to_build_dir: TARGET_NAME is required")
    endif()

    # Check if source directory exists
    if(NOT EXISTS "${ARGS_ASSETS_SOURCE_DIR}")
        message(WARNING "Assets source directory does not exist: ${ARGS_ASSETS_SOURCE_DIR}")
        return()
    endif()

    # Gather all asset files
    file(GLOB_RECURSE ASSET_FILES CONFIGURE_DEPENDS "${ARGS_ASSETS_SOURCE_DIR}/*")

    if(NOT ASSET_FILES)
        message(STATUS "No asset files found in: ${ARGS_ASSETS_SOURCE_DIR}")
        return()
    endif()

    # Create stamp file path for tracking
    set(ASSETS_STAMP_FILE "${CMAKE_CURRENT_BINARY_DIR}/assets_copied_${ARGS_TARGET_NAME}.stamp")
    set(ASSETS_DEST_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${ARGS_ASSETS_DEST_SUBDIR}")
    
    # Get current timestamp for logging
    string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S")

    # Add custom command with OUTPUT (avoids circular dependency)
    add_custom_command(
        OUTPUT ${ASSETS_STAMP_FILE}
        DEPENDS ${ASSET_FILES}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ASSETS_DEST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${ARGS_ASSETS_SOURCE_DIR}" "${ASSETS_DEST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E touch "${ASSETS_STAMP_FILE}"
        COMMAND ${CMAKE_COMMAND} -E echo "Assets copied: ${BUILD_DATE}" > "${ASSETS_STAMP_FILE}"
        COMMENT "Copying assets for ${ARGS_TARGET_NAME}: ${ARGS_ASSETS_SOURCE_DIR} → ${ASSETS_DEST_DIR}"
        VERBATIM
    )

    # Create a custom target that depends on the stamp file
    set(CUSTOM_TARGET_NAME "copy_assets_${ARGS_TARGET_NAME}")
    add_custom_target(${CUSTOM_TARGET_NAME} DEPENDS ${ASSETS_STAMP_FILE})

    # Add dependency to the main target
    add_dependencies(${ARGS_TARGET_NAME} ${CUSTOM_TARGET_NAME})

    message(STATUS "Asset copying configured for target '${ARGS_TARGET_NAME}' from: ${ARGS_ASSETS_SOURCE_DIR}")
endfunction()

