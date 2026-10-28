add_executable(kv_tests
  smoke_test.cpp)
target_link_libraries(kv_tests PRIVATE kv GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(kv_tests)
