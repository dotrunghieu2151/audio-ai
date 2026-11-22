# Prebuilt ONNX Runtime from the official NuGet package. vcpkg's builtin port builds from the
# GitHub sources, which cannot be downloaded (github.com/microsoft/onnxruntime returns 404 as of
# 2026-11-22). The package ships shared libraries only.
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_download_distfile(ARCHIVE
    URLS "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${VERSION}/microsoft.ml.onnxruntime.${VERSION}.nupkg"
    FILENAME "microsoft.ml.onnxruntime.${VERSION}.zip"
    SHA512 7e3d8ff6ce68dace4cbf0c1d0e2085e1faf05d425e77580b875c1bd22802527a276ebbdfa07326005b5a8fd6ad8186c17e5710b44dc543acc6bffb3be45404b1
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}" NO_REMOVE_ONE_LEVEL)

if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    set(arch arm64)
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    set(arch x64)
else()
    message(FATAL_ERROR "onnxruntime: no prebuilt binaries for ${VCPKG_TARGET_ARCHITECTURE}")
endif()

if(VCPKG_TARGET_IS_OSX)
    set(native "${SOURCE_PATH}/runtimes/osx-${arch}/native")
    # The library's install name is @rpath/libonnxruntime.1.dylib.
    set(runtime_files "${native}/libonnxruntime.dylib")
    set(runtime_name "libonnxruntime.1.dylib")
    set(link_name "libonnxruntime.dylib")
elseif(VCPKG_TARGET_IS_LINUX)
    set(native "${SOURCE_PATH}/runtimes/linux-${arch}/native")
    # SONAME libonnxruntime.so.1.
    set(runtime_files "${native}/libonnxruntime.so")
    set(runtime_name "libonnxruntime.so.1")
    set(link_name "libonnxruntime.so")
elseif(VCPKG_TARGET_IS_WINDOWS)
    set(native "${SOURCE_PATH}/runtimes/win-${arch}/native")
else()
    message(FATAL_ERROR "onnxruntime: unsupported target")
endif()

file(GLOB headers "${SOURCE_PATH}/build/native/include/*.h" "${SOURCE_PATH}/build/native/include/*.inc")
file(INSTALL ${headers} DESTINATION "${CURRENT_PACKAGES_DIR}/include/onnxruntime")

foreach(dir IN ITEMS "" "/debug")
    if(VCPKG_TARGET_IS_WINDOWS)
        file(INSTALL "${native}/onnxruntime.lib" "${native}/onnxruntime_providers_shared.lib"
             DESTINATION "${CURRENT_PACKAGES_DIR}${dir}/lib")
        file(INSTALL "${native}/onnxruntime.dll" "${native}/onnxruntime_providers_shared.dll"
             DESTINATION "${CURRENT_PACKAGES_DIR}${dir}/bin")
    else()
        file(INSTALL ${runtime_files} DESTINATION "${CURRENT_PACKAGES_DIR}${dir}/lib"
             RENAME "${runtime_name}")
        file(CREATE_LINK "${runtime_name}" "${CURRENT_PACKAGES_DIR}${dir}/lib/${link_name}" SYMBOLIC)
    endif()
endforeach()

configure_file("${CMAKE_CURRENT_LIST_DIR}/onnxruntimeConfig.cmake.in"
               "${CURRENT_PACKAGES_DIR}/share/${PORT}/onnxruntimeConfig.cmake" @ONLY)
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" "${SOURCE_PATH}/ThirdPartyNotices.txt")
