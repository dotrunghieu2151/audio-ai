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
# Add more test files here
set(TESTS
    mpmc_queue.test
    spsc_queue.test
)

foreach(t IN LISTS TESTS)
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} PRIVATE GTest::gtest_main DTH::common)
  gtest_discover_tests(${t})
endforeach()

# Link with Gtest
# target_link_libraries(AllTests PRIVATE GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main)



