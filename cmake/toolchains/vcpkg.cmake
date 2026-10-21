# =============================================================================
# VcpkgSetupToolchain.cmake
#
# A self-contained toolchain that ensures vcpkg is installed, synced to the
# correct baseline (from vcpkg-configuration.json), and bootstrapped.
#
# You can use this toolchain directly or include it from another toolchain.
# =============================================================================

# ---------------------------------------------------------------------------
# Configurable settings
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.31.10)

set(VCPKG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.vcpkg" CACHE PATH "Path to vcpkg root")
set(VCPKG_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/../../vcpkg-configuration.json" CACHE FILEPATH "vcpkg configuration JSON")
set(VCPKG_DEFAULT_REPO "https://github.com/microsoft/vcpkg" CACHE STRING "Default vcpkg repository URL")

# ---------------------------------------------------------------------------
# Read and validate vcpkg-configuration.json
# ---------------------------------------------------------------------------
if(NOT EXISTS "${VCPKG_CONFIG_FILE}")
    message(FATAL_ERROR "[vcpkg] Missing configuration file: ${VCPKG_CONFIG_FILE}")
endif()

# Parse registry kind, baseline, and optional repository
file(READ "${VCPKG_CONFIG_FILE}" VCPKG_CONFIG_JSON)

string(JSON VCPKG_KIND GET "${VCPKG_CONFIG_JSON}" "default-registry" "kind")
string(JSON VCPKG_BASELINE GET "${VCPKG_CONFIG_JSON}" "default-registry" "baseline")


if("${VCPKG_KIND}" STREQUAL "git")
    string(JSON VCPKG_REPO_URL GET "${VCPKG_CONFIG_JSON}" "default-registry" "repository")
elseif("${VCPKG_KIND}" STREQUAL "builtin")
    set(VCPKG_REPO_URL "${VCPKG_DEFAULT_REPO}")
else()
    message(FATAL_ERROR "[vcpkg] Unknown registry kind '${VCPKG_KIND}' in ${VCPKG_CONFIG_FILE}")
endif()

if(NOT VCPKG_BASELINE OR "${VCPKG_BASELINE}" STREQUAL "null")
    message(FATAL_ERROR "[vcpkg] Invalid configuration: missing baseline")
endif()

if(NOT VCPKG_REPO_URL OR "${VCPKG_REPO_URL}" STREQUAL "null")
    message(FATAL_ERROR "[vcpkg] Invalid configuration: missing repository URL")
endif()

message(STATUS "[vcpkg] Registry: ${VCPKG_REPO_URL}")
message(STATUS "[vcpkg] Baseline: ${VCPKG_BASELINE}")

# ---------------------------------------------------------------------------
# Clone vcpkg if missing
# ---------------------------------------------------------------------------
if(NOT EXISTS "${VCPKG_ROOT}/.git")
    message(STATUS "[vcpkg] Cloning repository into ${VCPKG_ROOT}")
    execute_process(
        COMMAND git clone "${VCPKG_REPO_URL}" "${VCPKG_ROOT}"
        RESULT_VARIABLE GIT_CLONE_RESULT
    )
    if(NOT GIT_CLONE_RESULT EQUAL 0)
        message(FATAL_ERROR "[vcpkg] Failed to clone repository from ${VCPKG_REPO_URL}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Fetch and ensure baseline
# ---------------------------------------------------------------------------
execute_process(
    COMMAND git fetch origin
    WORKING_DIRECTORY "${VCPKG_ROOT}"
    RESULT_VARIABLE GIT_FETCH_RESULT
    OUTPUT_QUIET ERROR_QUIET
)
if(NOT GIT_FETCH_RESULT EQUAL 0)
    message(WARNING "[vcpkg] Failed to fetch latest changes (offline mode?)")
endif()

execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${VCPKG_ROOT}"
    OUTPUT_VARIABLE CURRENT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT "${CURRENT_COMMIT}" STREQUAL "${VCPKG_BASELINE}")
    message(STATUS "[vcpkg] Checking out baseline ${VCPKG_BASELINE}")
    execute_process(
        COMMAND git checkout ${VCPKG_BASELINE}
        WORKING_DIRECTORY "${VCPKG_ROOT}"
        RESULT_VARIABLE GIT_CHECKOUT_RESULT
        OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT GIT_CHECKOUT_RESULT EQUAL 0)
        message(FATAL_ERROR "[vcpkg] Failed to checkout baseline ${VCPKG_BASELINE}")
    endif()
else()
    message(STATUS "[vcpkg] Already at baseline ${VCPKG_BASELINE}")
endif()

# ---------------------------------------------------------------------------
# Bootstrap vcpkg if necessary
# ---------------------------------------------------------------------------
if(WIN32)
    set(VCPKG_BOOTSTRAP "${VCPKG_ROOT}/bootstrap-vcpkg.bat")
    set(VCPKG_EXE "${VCPKG_ROOT}/vcpkg.exe")
else()
    set(VCPKG_BOOTSTRAP "${VCPKG_ROOT}/bootstrap-vcpkg.sh")
    set(VCPKG_EXE "${VCPKG_ROOT}/vcpkg")
endif()

if(NOT EXISTS "${VCPKG_EXE}" OR NOT EXISTS "${VCPKG_BOOTSTRAP}" OR NOT "${CURRENT_COMMIT}" STREQUAL "${VCPKG_BASELINE}")
    message(STATUS "[vcpkg] Bootstrapping...")
    if(WIN32)
        execute_process(COMMAND cmd /c "${VCPKG_BOOTSTRAP}" -disableMetrics WORKING_DIRECTORY "${VCPKG_ROOT}")
    else()
        execute_process(COMMAND "${VCPKG_BOOTSTRAP}" -disableMetrics WORKING_DIRECTORY "${VCPKG_ROOT}")
    endif()
else()
    message(STATUS "[vcpkg] Already bootstrapped.")
endif()

# Register local custom registry
set(VCPKG_FEATURE_FLAGS "registries" CACHE STRING "Enable custom registries" FORCE)
set(VCPKG_OVERLAY_REGISTRIES "${CMAKE_CURRENT_LIST_DIR}/../../vcpkg-registry")

# ---------------------------------------------------------------------------
# Integrate toolchain
# ---------------------------------------------------------------------------
message(STATUS "Including vcpkg toolchain ${VCPKG_ROOT}")
include("${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")

set(VCPKG_BOOTSTRAPPED TRUE CACHE BOOL "vcpkg is installed and ready")
message(STATUS "[vcpkg] ✅ Setup complete at ${VCPKG_ROOT}")
message(STATUS "[vcpkg] ✅ Baseline: ${VCPKG_BASELINE}")
