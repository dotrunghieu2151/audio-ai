set(VCPKG_POLICY_CMAKE_HELPER_PORT enabled)

# Detect platform
if(VCPKG_TARGET_IS_WINDOWS)
    set(RUST_VERSION "1.92.0")
    set(RUST_URL "https://static.rust-lang.org/dist/rust-${RUST_VERSION}-x86_64-pc-windows-msvc.tar.gz")
    set(RUST_FILENAME "rust-${RUST_VERSION}-x86_64-windows.tar.gz")
    set(RUST_SHA512 "7ac1d4a8c6f0492e573186bb0a03dea6a13f3df6e688bc055ca3af474d5f0eacb939273a9bc8d5df0128059e8b39aca1cd0d296c29db54efb6dd7f71277c83e1")
elseif(VCPKG_TARGET_IS_LINUX)
    set(RUST_VERSION "1.92.0")
    set(RUST_URL "https://static.rust-lang.org/dist/rust-${RUST_VERSION}-x86_64-unknown-linux-gnu.tar.gz")
    set(RUST_FILENAME "rust-${RUST_VERSION}-x86_64-linux.tar.gz")
    set(RUST_SHA512 "6a2e4c2996726815efdb1ce89305bbcba863dad46f1510dd57f9732fc689f0bbccdd453ac1984537a77fa90c5e19c1b73d27acb5f976481ab020b9eeeb102e49")
elseif(VCPKG_TARGET_IS_OSX)
    set(RUST_VERSION "1.92.0")
    # Detect architecture: check CMAKE_OSX_ARCHITECTURES or system architecture
    if(VCPKG_TARGET_ARCHITECTURE MATCHES "arm64")
        set(RUST_ARCH "aarch64")
        set(RUST_URL "https://static.rust-lang.org/dist/rust-${RUST_VERSION}-aarch64-apple-darwin.tar.gz")
        set(RUST_FILENAME "rust-${RUST_VERSION}-aarch64-macos.tar.gz")
        set(RUST_SHA512 "367f2f9c68cac18b7dbe6efec60f110505a1d33a729a43d3d46a14d35d95efa6008ef0e9b87c33c92f8684e965fcac6922347186b0c427dd526ca06ff9883c06")
    else()
        set(RUST_ARCH "x86_64")
        set(RUST_URL "https://static.rust-lang.org/dist/rust-${RUST_VERSION}-x86_64-apple-darwin.tar.gz")
        set(RUST_FILENAME "rust-${RUST_VERSION}-x86_64-macos.tar.gz")
        set(RUST_SHA512 "2f9610a73048efac56d2e253eb8b800ee7bb07b279b3cfedd7ce9c35d128572c44b32b10f6be0e21a88b669641bca15170c5230143841e5efe8d370d8809a0ce")
    endif()
else()
    message(FATAL_ERROR "Unsupported platform for Rust")
endif()

# Download appropriate Rust package
vcpkg_download_distfile(
    RUST_INSTALLER
    URLS "${RUST_URL}"
    FILENAME "${RUST_FILENAME}"
    SHA512 ${RUST_SHA512}
)

# Extract
vcpkg_extract_source_archive_ex(
    OUT_SOURCE_PATH SOURCE_PATH
    ARCHIVE "${RUST_INSTALLER}"
)

if(VCPKG_TARGET_IS_OSX OR VCPKG_TARGET_IS_LINUX) 
    # issue: https://discourse.cmake.org/t/problem-with-rpath-setting-in-executable-built-as-a-vcpkg-port/14320
    # basically vcpkg is setting the incorrect rpath for executables
    set(VCPKG_FIXUP_MACHO_RPATH OFF)
endif()

# Copy everything to tools directory
# rustc binaries
file(INSTALL
    DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin"
    TYPE DIRECTORY
    FILES "${SOURCE_PATH}/rustc/bin/rustc"
)

# rustc internal libraries
file(INSTALL
    DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/lib"
    TYPE DIRECTORY
    FILES "${SOURCE_PATH}/rustc/lib/"
)

# Find the rust-std component directory
file(GLOB RUST_STD_DIRS "${SOURCE_PATH}/rust-std-*")
list(LENGTH RUST_STD_DIRS RUST_STD_COUNT)
if(RUST_STD_COUNT EQUAL 0)
    message(FATAL_ERROR "Could not find rust-std component for ${RUST_TARGET}")
elseif(RUST_STD_COUNT GREATER 1)
    message(WARNING "Multiple rust-std directories found, using first: ${RUST_STD_DIRS}")
endif()
list(GET RUST_STD_DIRS 0 RUST_STD_DIR)

# Copy standard library into rustc's lib directory
# The rust-std component contains lib/rustlib/ which needs to be merged
if(EXISTS "${RUST_STD_DIR}/lib/rustlib")
    file(INSTALL
        DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/lib"
        TYPE DIRECTORY
        FILES "${RUST_STD_DIR}/lib/rustlib"
    )
else()
    message(FATAL_ERROR "rust-std component does not contain lib/rustlib directory")
endif()

# cargo binaries
file(INSTALL
    DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin"
    TYPE DIRECTORY
    FILES "${SOURCE_PATH}/cargo/bin/"
)

# fix @rpath on macos
if(VCPKG_TARGET_IS_OSX)
    message(STATUS "Fixing rustc rpath...")

    set(RUST_BIN_DIR "${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin")

    foreach(bin rustc cargo)
        set(target "${RUST_BIN_DIR}/${bin}")

        if(EXISTS "${target}")
            # Remove incorrect rpath if present
            execute_process(
                COMMAND install_name_tool
                    -delete_rpath @loader_path/../../../lib
                    "${target}"
                RESULT_VARIABLE _res
                ERROR_QUIET
            )

            # Ignore failure (path may not exist)
        endif()
    endforeach()
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/rustbinConfig.cmake"
               "${CURRENT_PACKAGES_DIR}/share/${PORT}/rustbinConfig.cmake"
               @ONLY)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/vcpkg-port-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

               # Fix permissions cross-platform
if(VCPKG_TARGET_IS_LINUX OR VCPKG_TARGET_IS_OSX)
    message(STATUS "Fixing executable permissions for Rust binaries...")
    file(GLOB_RECURSE RUST_BINARIES "${CURRENT_PACKAGES_DIR}/tools/${PORT}/*")
    foreach(f ${RUST_BINARIES})
        file(CHMOD ${f}
            PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE
        )
    endforeach()

    # if(APPLE)
    #     # Remove Gatekeeper quarantine
    #     execute_process(
    #         COMMAND xattr -dr com.apple.quarantine "${CURRENT_PACKAGES_DIR}/tools/rust/bin"
    #         COMMAND_ERROR_IS_FATAL ANY
    #     )
    # endif()
endif()

# Install license info (if available)
if(EXISTS "${RUST_ROOT}/LICENSE.txt")
    vcpkg_install_copyright(FILE_LIST "${RUST_ROOT}/LICENSE.txt")
endif()

message(STATUS "Rust ${RUST_VERSION} installed for ${VCPKG_TARGET_TRIPLET}")
