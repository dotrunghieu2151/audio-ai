# --- CPPCheck works cross-platform ---
find_program(CPPCHECK NAMES cppcheck)
if (CPPCHECK)
    message(STATUS "Found cppcheck: ${CPPCHECK}")
    set(CMAKE_CXX_CPPCHECK "${CPPCHECK};--enable=all;--error-exitcode=1;--std=c++20;--suppress=missingIncludeSystem")
else()
    message(ERROR "cppcheck not found! Skipping lint integration.")
endif()