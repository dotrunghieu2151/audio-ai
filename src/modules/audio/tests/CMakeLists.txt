find_package(GTest QUIET)

if (NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/063de7e9578f82b369302001269680b4b1553359.zip
  )
  FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# Replaces the global operator new/delete of every test binary that links it.
add_library(dth_audio_rt_alloc_guard OBJECT rt_alloc_guard.cpp)
target_compile_features(dth_audio_rt_alloc_guard PUBLIC cxx_std_23)
target_include_directories(dth_audio_rt_alloc_guard PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if (DTH_ENABLE_SANITIZERS)
  target_link_libraries(dth_audio_rt_alloc_guard PUBLIC DTH::sanitizers)
endif()

# Add more test files here
set(TESTS
    rt_alloc_guard.test
    pipeline_contract.test
    format_converter.test
    drift_controller.test
    pipeline_stream.test
)

foreach(t IN LISTS TESTS)
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} PRIVATE GTest::gtest_main DTH::audio dth_audio_rt_alloc_guard)
  # Internal (non-public) headers, e.g. pipeline/.
  target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)

  if (DTH_ENABLE_SANITIZERS)
    target_link_libraries(${t} PUBLIC DTH::sanitizers)
  endif()
  gtest_discover_tests(${t})
endforeach()

# Tests that need a real audio device. CI: ctest -LE device
set(DEVICE_TESTS
    device_output_sink.test
    device_input_source.test
    system_audio_source.test
    system_audio_stream.test
)

foreach(t IN LISTS DEVICE_TESTS)
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} PRIVATE GTest::gtest_main DTH::audio DTH::common dth_audio_rt_alloc_guard)
  target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)

  if (DTH_ENABLE_SANITIZERS)
    target_link_libraries(${t} PUBLIC DTH::sanitizers)
  endif()
  gtest_discover_tests(${t} PROPERTIES LABELS device)
endforeach()
