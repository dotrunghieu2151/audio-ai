vcpkg_from_github(
    OUT_SOURCE_PATH CORROSION_SOURCE
    REPO "corrosion-rs/corrosion"
    REF "v0.6.1"
    SHA512 2b0d1ccafd5472f2938d084995662c586f19cb5cd4ead20fa25e9516d595eb5f756cb2d9abb12dccfa944b52a1f8002c69a1a4d955409c9d194c6d885a6d48ca
)

message(STATUS "Rust compiler: ${RUSTBIN_RUSTC_EXECUTABLE}")
message(STATUS "Rust cargo: ${RUSTBIN_CARGO_EXECUTABLE}")

vcpkg_cmake_configure(
    SOURCE_PATH "${CORROSION_SOURCE}"
    OPTIONS
      -DRust_COMPILER=${RUSTBIN_RUSTC_EXECUTABLE}
      -DRust_CARGO=${RUSTBIN_CARGO_EXECUTABLE}
      -DCORROSION_BUILD_TESTS=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME Corrosion CONFIG_PATH lib/cmake/Corrosion)
