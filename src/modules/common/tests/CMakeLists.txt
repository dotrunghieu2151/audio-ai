SET(all_files_in_test_dir
  ScspQueue.test.cpp
  McmpQueue.test.cpp
)
add_executable(AllTests ${all_files_in_test_dir})
target_link_libraries(AllTests PRIVATE GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main)
target_link_libraries(AllTests PUBLIC Core)

add_test(AllTests AllTests)
