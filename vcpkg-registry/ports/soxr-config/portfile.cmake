configure_file("${CMAKE_CURRENT_LIST_DIR}/soxrConfig.cmake"
               "${CURRENT_PACKAGES_DIR}/share/soxr/soxrConfig.cmake"
               @ONLY)

configure_file("${CMAKE_CURRENT_LIST_DIR}/soxrConfigVersion.cmake"
"${CURRENT_PACKAGES_DIR}/share/soxr/soxrConfigVersion.cmake"
@ONLY)
