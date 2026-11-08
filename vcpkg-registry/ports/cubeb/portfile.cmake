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
    REPO mozilla/cubeb
    REF dcd7cddafa4b95776cf5d117549ad58e17f23764
    SHA512 df3958b82d6fc93527638ca7e0569e8e8bf96f121f736e98eb69aa265de4247b0f9d235ef4612556708db51de4f8536ab2a607cfc0f5ae8674b198f1da615688
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        use-rust-libs BUILD_RUST_LIBS
)

if ("use-rust-libs" IN_LIST FEATURES)
  # Build rust libs
  # Locate Cargo.
  find_program(CARGO_EXECUTABLE cargo REQUIRED)
  # Fetch Rust backend sources.
  # These are git submodules in Mozilla/cubeb.
  if (VCPKG_TARGET_IS_OSX)
    vcpkg_from_git(
    OUT_SOURCE_PATH COREAUDIO_RS_SOURCE
    URL https://github.com/mozilla/cubeb-coreaudio-rs
    REF 840f69c1b3f95accd582555e10abcef8ee67514b
)
    # Populate the expected submodule directories.
    file(
    COPY "${COREAUDIO_RS_SOURCE}/"
    DESTINATION "${SOURCE_PATH}/src/cubeb-coreaudio-rs"
)
    # Build Debug.
    vcpkg_execute_required_process(
        COMMAND
            "${CARGO_EXECUTABLE}"
            build
            --offline
            --features=gecko-in-tree
        WORKING_DIRECTORY
            "${SOURCE_PATH}/src/cubeb-coreaudio-rs"
        LOGNAME
            cargo-build-coreaudio-debug
    )

    # Build Release
    vcpkg_execute_required_process(
        COMMAND
            "${CARGO_EXECUTABLE}"
            build
            --offline
            --release
            --features=gecko-in-tree
        WORKING_DIRECTORY
            "${SOURCE_PATH}/src/cubeb-coreaudio-rs"
        LOGNAME
            cargo-build-coreaudio-release
    )
  endif()

  if (VCPKG_TARGET_IS_LINUX)
    vcpkg_from_git(
    OUT_SOURCE_PATH PULSE_RS_SOURCE
    URL https://github.com/mozilla/cubeb-pulse-rs
    REF e5e0b0ba8b02f1d017ee2f3e2ec47c33da865131
)
    file(
    COPY "${PULSE_RS_SOURCE}/"
    DESTINATION "${SOURCE_PATH}/src/cubeb-pulse-rs"
)

    # Build Debug.
    vcpkg_execute_required_process(
        COMMAND
            "${CARGO_EXECUTABLE}"
            build
            --offline
            --features=gecko-in-tree
        WORKING_DIRECTORY
            "${SOURCE_PATH}/src/cubeb-pulse-rs"
        LOGNAME
            cargo-build-pulse-debug
    )

    # Build Release
    vcpkg_execute_required_process(
        COMMAND
            "${CARGO_EXECUTABLE}"
            build
            --offline
            --release
            --features=gecko-in-tree
        WORKING_DIRECTORY
            "${SOURCE_PATH}/src/cubeb-pulse-rs"
        LOGNAME
            cargo-build-pulse-release
    )

  endif()
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        # -DRust_COMPILER=${RUSTBIN_RUSTC_EXECUTABLE}
        # -DRust_CARGO=${RUSTBIN_CARGO_EXECUTABLE}
        -DBUILD_TESTS=OFF
        -DUSE_SANITIZERS=OFF
        -DBUILD_SHARED_LIBS=ON
)

message(STATUS "Configuring Cubeb ${CURRENT_PACKAGES_DIR}")

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME cubeb
    CONFIG_PATH lib/cmake/cubeb
)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

