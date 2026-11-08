# ==============================
# Portfile for Slint (custom)
# ==============================
cmake_minimum_required(VERSION 3.31.10)
# Static libslint_cpp.a is installed without its native link deps (macOS frameworks,
# Linux fontconfig, ...): Corrosion only attaches Rust std libs and SlintConfig.cmake
# drops even those. A cdylib links them itself, so build shared (Slint's own default).
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

# 1. Download Slint source
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO slint-ui/slint
    REF v1.18.1
    SHA512 e515249390aaed5b3bc6d3ef41222910ef7e36cf2937472aae28e18e6480d33ca63d9d0c7f41bc7895ac8a5f96bbbd3147af2f54cfa3a52367ba52c7ec50ebea
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        live-preview SLINT_FEATURE_LIVE_PREVIEW
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/api/cpp"
    OPTIONS
        ${FEATURE_OPTIONS}
        # -DRust_COMPILER=${RUSTBIN_RUSTC_EXECUTABLE}
        # -DRust_CARGO=${RUSTBIN_CARGO_EXECUTABLE}
)

message(STATUS "Configuring Slint ${CURRENT_PACKAGES_DIR}")

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME slint
    CONFIG_PATH lib/cmake/Slint
)

# SlintConfig.cmake only sets a config-less IMPORTED_LOCATION pointing at the release
# lib/. Map Debug to debug/lib so Debug consumers don't mix release Slint with other
# ports' debug libs (causes "cycle in the constraint graph" rpath warnings).
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/slint/SlintConfig.cmake"
    "\nset(_IMPORT_PREFIX)\n"
    [[

foreach(_slint_prop IMPORTED_LOCATION IMPORTED_IMPLIB)
  get_target_property(_slint_val slint_cpp-shared ${_slint_prop})
  if(_slint_val)
    string(REPLACE "${_IMPORT_PREFIX}/lib/" "${_IMPORT_PREFIX}/debug/lib/" _slint_dbg "${_slint_val}")
    if(EXISTS "${_slint_dbg}")
      set_target_properties(slint_cpp-shared PROPERTIES ${_slint_prop}_DEBUG "${_slint_dbg}")
    endif()
  endif()
endforeach()
unset(_slint_prop)
unset(_slint_val)
unset(_slint_dbg)

set(_IMPORT_PREFIX)
]]
)
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSES")
