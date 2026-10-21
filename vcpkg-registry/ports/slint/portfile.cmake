# ==============================
# Portfile for Slint (custom)
# ==============================
cmake_minimum_required(VERSION 3.31.10)
# 1. Download Slint source
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO slint-ui/slint
    REF v1.15.1
    SHA512 c2d890cf3dd9b871dededbc3bd7301e00a34d492421bb0fb15f010a8f76826a4a4030285e48105c164a5095b9893186b76515c1bd860bb5a57fc3af90a7823bd
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/api/cpp"
    OPTIONS
        -DSLINT_FEATURE_LIVE_PREVIEW=ON
        -DRust_COMPILER=${RUSTBIN_RUSTC_EXECUTABLE}
        -DRust_CARGO=${RUSTBIN_CARGO_EXECUTABLE}
)

message(STATUS "Configuring Slint ${CURRENT_PACKAGES_DIR}")

vcpkg_cmake_install()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSES")