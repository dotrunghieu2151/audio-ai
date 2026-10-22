# vcpkg integration
if (NOT DEFINED ENV{VCPKG_ROOT})
  message(
  FATAL_ERROR
  "VCPKG executable not found via VCPKG_ROOT env: $ENV{VCPKG_ROOT}.
  Please install vcpkg or use the install-vcpkg.sh script provided by the project"
)
else()
  # if VCPKG_ROOT is set, use the prebuilt vcpkg
  include("$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
  message(STATUS "Using prebuilt vcpkg.cmake")
endif()
