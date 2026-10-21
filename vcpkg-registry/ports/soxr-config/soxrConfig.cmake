# soxrConfig.cmake
# Custom config to make find_package(soxr) work

# Search for the header and library (your existing code)
find_path(SOXR_INCLUDE_DIR soxr.h)
find_library(SOXR_LIBRARY NAMES soxr)

if(SOXR_INCLUDE_DIR AND SOXR_LIBRARY)
    # Create an imported target
    add_library(soxr::soxr UNKNOWN IMPORTED)
    set_target_properties(soxr::soxr PROPERTIES
        IMPORTED_LOCATION "${SOXR_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SOXR_INCLUDE_DIR}"
    )
    # Optionally, provide variables as well
    set(SOXR_FOUND TRUE)
    set(SOXR_INCLUDE_DIRS "${SOXR_INCLUDE_DIR}")
    set(SOXR_LIBRARIES "${SOXR_LIBRARY}")
else()
    message(FATAL_ERROR "Could not find SoXR library. Make sure it's installed.")
endif()
