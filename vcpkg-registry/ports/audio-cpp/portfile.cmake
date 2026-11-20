# The only consumable library is the C API, which upstream builds as a shared
# library that absorbs the vendored ggml/sentencepiece/... and exports only
# audiocpp_* symbols. A static build would need every internal archive
# installed, and its vendored ggml would collide with the ggml port.
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO 0xShug0/audio.cpp
    REF "v${VERSION}"
    SHA512 72da71535d1c96bd0a258ecf1b6ad78409ab1559dc6e1aad609a8868cdb6957e5776cc01903725bcb5e2486018021bf080252d302e2877a8a153b1bfdbc2ba5b
    HEAD_REF main
    PATCHES
        vcpkg-integration.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        cuda            ENGINE_ENABLE_CUDA
        hip             ENGINE_ENABLE_HIP
        hip-strix-halo  ENGINE_HIP_STRIX_HALO_OPTIMIZATIONS
        metal           ENGINE_ENABLE_METAL
        sycl            ENGINE_ENABLE_SYCL
        vulkan          ENGINE_ENABLE_VULKAN
        webgpu          ENGINE_ENABLE_WEBGPU
        rknn            ENGINE_ENABLE_RKNN
        neon            ENGINE_ENABLE_NEON
        native-cpu      ENGINE_ENABLE_NATIVE_CPU
        openmp          ENGINE_ENABLE_OPENMP
        openmp          GGML_OPENMP
        espeak          AUDIOCPP_STATIC_ESPEAK
        model-manager   AUDIOCPP_BUILD_NATIVE_MODEL_MANAGER
        zonos3-textnorm AUDIOCPP_ZONOS3_TEXTNORM
)

if("cuda" IN_LIST FEATURES AND "hip" IN_LIST FEATURES)
    message(FATAL_ERROR "Features 'cuda' and 'hip' drive the same ggml backend and are mutually exclusive.")
endif()

if("cuda" IN_LIST FEATURES)
    vcpkg_find_cuda(OUT_CUDA_TOOLKIT_ROOT cuda_toolkit_root)
    list(APPEND FEATURE_OPTIONS
        "-DCMAKE_CUDA_COMPILER=${NVCC}"
        "-DCUDAToolkit_ROOT=${cuda_toolkit_root}"
    )
endif()

if(NOT "native-cpu" IN_LIST FEATURES)
    # Without GGML_NATIVE, ggml enables AVX/AVX2/FMA/F16C/BMI2 by default for
    # native builds only; pin the baseline so binaries match the ggml port and
    # don't depend on whether the build was cross-compiled.
    list(APPEND FEATURE_OPTIONS
        -DGGML_AVX=OFF
        -DGGML_AVX2=OFF
        -DGGML_AVX512=OFF
        -DGGML_BMI2=OFF
        -DGGML_FMA=OFF
        -DGGML_F16C=OFF
        -DGGML_SSE42=OFF
    )
endif()

if("webgpu" IN_LIST FEATURES)
    # ggml-webgpu embeds its WGSL shaders with a Python script.
    vcpkg_find_acquire_program(PYTHON3)
    list(APPEND FEATURE_OPTIONS "-DPython3_EXECUTABLE=${PYTHON3}")
endif()

if("vulkan" IN_LIST FEATURES)
    list(APPEND FEATURE_OPTIONS
        "-DVulkan_GLSLC_EXECUTABLE=${CURRENT_HOST_INSTALLED_DIR}/tools/shaderc/glslc${VCPKG_HOST_EXECUTABLE_SUFFIX}"
    )
endif()

if("espeak" IN_LIST FEATURES)
    # eSpeak's data compiler copies its data directory into a fixed buffer
    # (N_PATH_HOME: 160, 230 on Windows); a longer path silently truncates and
    # the build aborts.
    set(espeak_path_limit 160)
    if(VCPKG_TARGET_IS_WINDOWS)
        set(espeak_path_limit 230)
    endif()
    set(espeak_data_dir "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-dbg/_deps/espeak-static/build/espeak-ng-data")
    string(LENGTH "${espeak_data_dir}" espeak_data_dir_length)
    if(NOT espeak_data_dir_length LESS espeak_path_limit)
        message(FATAL_ERROR "Feature 'espeak' needs a shorter buildtrees path: "
            "'${espeak_data_dir}' is ${espeak_data_dir_length} characters, eSpeak-ng allows fewer than ${espeak_path_limit}. "
            "Use a shorter vcpkg root or --x-buildtrees-root.")
    endif()
    # Upstream fetches this at build time; pre-fetch so the build stays offline.
    vcpkg_download_distfile(ESPEAK_ARCHIVE
        URLS "https://codeload.github.com/espeak-ng/espeak-ng/tar.gz/refs/tags/1.52.0"
        FILENAME "espeak-ng-1.52.0.tar.gz"
        SHA512 28793503f2be00505c3fe1dada8197a4e2d3af0d143e31ef883130fe2685d048ae9dee8efab637cdbe972461d95e210d2277b8e8fed4ae580d0cca8b505b0662
    )
    vcpkg_extract_source_archive(ESPEAK_SOURCE_PATH ARCHIVE "${ESPEAK_ARCHIVE}")
    list(APPEND FEATURE_OPTIONS "-DAUDIOCPP_ESPEAK_ARCHIVE=${ESPEAK_ARCHIVE}")
endif()

if("zonos3-textnorm" IN_LIST FEATURES)
    vcpkg_from_github(
        OUT_SOURCE_PATH TEXTNORM_SOURCE_PATH
        REPO Zyphra/zyphra-textnorm
        REF v0.1.0
        SHA512 d9e0c46c545870be3311db5d64d366ca2c1fd624c37932ff0b8472a8b43cfb9d595b8ab9655ddf89e9880cb5df89921d9b31a04e216989c384e433b8ca65069a
        HEAD_REF main
    )
    # vcpkg strips CARGO_HOME unless listed in VCPKG_KEEP_ENV_VARS.
    set(cargo_hints "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")
    find_program(CARGO_EXECUTABLE cargo HINTS ${cargo_hints} REQUIRED)
    find_program(RUSTC_EXECUTABLE rustc HINTS ${cargo_hints} REQUIRED)
    list(APPEND FEATURE_OPTIONS
        "-DAUDIOCPP_ZONOS3_TEXTNORM_DIR=${TEXTNORM_SOURCE_PATH}"
        "-DAUDIOCPP_CARGO=${CARGO_EXECUTABLE}"
        "-DAUDIOCPP_RUSTC=${RUSTC_EXECUTABLE}"
    )
endif()

# Optional in-process audiocpp_server frontends live in a separate repository
# (a git submodule upstream, so absent from the release tarball).
set(SERVER_FRONTENDS "")
foreach(frontend IN ITEMS audio-decode mp3-encode https websocket)
    if("server-${frontend}" IN_LIST FEATURES)
        string(REPLACE "-" "_" module "${frontend}")
        list(APPEND SERVER_FRONTENDS "${module}")
    endif()
endforeach()
if(SERVER_FRONTENDS)
    vcpkg_from_github(
        OUT_SOURCE_PATH FRONTENDS_SOURCE_PATH
        REPO 0xShug0/audio.cpp-server-frontends
        REF 070e69508a17b9b165f6d3ab625f497bf8995461
        SHA512 539ec472faa85eb6e92ef71ec46f18b4bfee042295a2c065f0869ef1aa283bd5478829f33aae793fef0ba7a3a465b7cdcc70f197d612662b73ce7ae5b6f0bd0b
        HEAD_REF main
    )
    string(REPLACE ";" "\\;" SERVER_FRONTENDS_ARG "${SERVER_FRONTENDS}")
    list(APPEND FEATURE_OPTIONS
        -DAUDIOCPP_BUILD_SERVER_FRONTENDS=ON
        "-DAUDIOCPP_SERVER_FRONTENDS_DIR=${FRONTENDS_SOURCE_PATH}"
        "-DAUDIOCPP_SERVER_FRONTEND_MODULES=${SERVER_FRONTENDS_ARG}"
    )
endif()

# TLS (model manager, HTTPS/WebSocket frontends) uses vcpkg's OpenSSL instead
# of upstream's build-time BoringSSL download.
if("model-manager" IN_LIST FEATURES OR "server-https" IN_LIST FEATURES OR "server-websocket" IN_LIST FEATURES)
    list(APPEND FEATURE_OPTIONS -DAUDIOCPP_USE_SYSTEM_OPENSSL=ON)
endif()

set(OPTIONS_RELEASE "")
set(OPTIONS_DEBUG "")
if("server-mp3-encode" IN_LIST FEATURES AND VCPKG_TARGET_IS_WINDOWS)
    # find_library(NAMES mp3lame) matches neither of the mp3lame port's names:
    # libmp3lame.lib (dynamic) or libmp3lame-static.lib (static).
    set(lame_name libmp3lame.lib)
    if(NOT EXISTS "${CURRENT_INSTALLED_DIR}/lib/${lame_name}")
        set(lame_name libmp3lame-static.lib)
    endif()
    list(APPEND OPTIONS_RELEASE "-DAUDIOCPP_LAME_LIBRARY=${CURRENT_INSTALLED_DIR}/lib/${lame_name}")
    list(APPEND OPTIONS_DEBUG "-DAUDIOCPP_LAME_LIBRARY=${CURRENT_INSTALLED_DIR}/debug/lib/${lame_name}")
endif()
if("zonos3-textnorm" IN_LIST FEATURES AND VCPKG_TARGET_IS_WINDOWS)
    # Rust staticlibs always link the release CRT; mixing it into the /MDd
    # debug DLL would load two C runtimes. Debug builds skip text normalization.
    list(APPEND OPTIONS_DEBUG -DAUDIOCPP_ZONOS3_TEXTNORM=OFF)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        "-DAUDIOCPP_VERSION=${VERSION}"
        # Packaged binaries must not depend on a source checkout's model_specs/.
        -DAUDIOCPP_DEPLOYMENT_BUILD=ON
        -DAUDIOCPP_BUILD_C_API=ON
        -DENGINE_ENABLE_CPU_ALL_VARIANTS=OFF
        -DENGINE_BUILD_EXAMPLES=OFF
        -DENGINE_BUILD_TESTS=OFF
        -DENGINE_BUILD_EXTENDED_TESTS=OFF
        -DENGINE_BUILD_MODEL_TESTS=OFF
        -DENGINE_BUILD_WARMBENCH=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_Git=ON
    OPTIONS_RELEASE ${OPTIONS_RELEASE}
    OPTIONS_DEBUG ${OPTIONS_DEBUG}
    MAYBE_UNUSED_VARIABLES
        AUDIOCPP_CARGO
        AUDIOCPP_RUSTC
        AUDIOCPP_LAME_LIBRARY
        GGML_OPENMP
        GGML_FMA
        GGML_F16C
        Python3_EXECUTABLE
        Vulkan_GLSLC_EXECUTABLE
)

vcpkg_cmake_build()

# Install only the "audiocpp" component: the vendored ggml and sentencepiece
# carry their own install rules, which would collide with the ggml and
# sentencepiece ports.
foreach(build_type IN ITEMS rel dbg)
    set(build_dir "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-${build_type}")
    if(EXISTS "${build_dir}")
        vcpkg_execute_required_process(
            COMMAND "${CMAKE_COMMAND}" --install . --component audiocpp
            WORKING_DIRECTORY "${build_dir}"
            LOGNAME "install-${TARGET_TRIPLET}-${build_type}"
        )
    endif()
endforeach()

vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME audiocpp CONFIG_PATH lib/cmake/audiocpp)

set(TOOL_NAMES audiocpp_cli audiocpp_server audiocpp_gguf)
if("model-manager" IN_LIST FEATURES)
    list(APPEND TOOL_NAMES audiocpp_model_manager)
endif()
if("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES ${TOOL_NAMES} AUTO_CLEAN)
    if("espeak" IN_LIST FEATURES)
        # Static eSpeak looks for its data next to the running executable.
        file(INSTALL "${CURRENT_PACKAGES_DIR}/share/audiocpp/espeak-ng-data.bin"
            DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
    endif()
else()
    vcpkg_clean_executables_in_bin(FILE_NAMES ${TOOL_NAMES})
endif()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

set(LICENSE_FILES "${SOURCE_PATH}/LICENSE")
if(SERVER_FRONTENDS)
    list(APPEND LICENSE_FILES "${FRONTENDS_SOURCE_PATH}/LICENSE")
endif()
if("espeak" IN_LIST FEATURES)
    list(APPEND LICENSE_FILES "${ESPEAK_SOURCE_PATH}/COPYING")
endif()
if("zonos3-textnorm" IN_LIST FEATURES)
    file(GLOB TEXTNORM_THIRD_PARTY_LICENSES "${TEXTNORM_SOURCE_PATH}/licenses/*")
    list(APPEND LICENSE_FILES "${TEXTNORM_SOURCE_PATH}/LICENSE" ${TEXTNORM_THIRD_PARTY_LICENSES})
endif()
vcpkg_install_copyright(FILE_LIST ${LICENSE_FILES})
