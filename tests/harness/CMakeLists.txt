add_executable(kv_tests
  smoke_test.cpp
  memtable_test.cpp
  validation_test.cpp
  coding_test.cpp
  crc32c_test.cpp
  wal_codec_test.cpp
  posix_file_test.cpp)
target_link_libraries(kv_tests PRIVATE kv GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(kv_tests)
