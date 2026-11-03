add_executable(kv_tests
  smoke_test.cpp
  memtable_test.cpp
  validation_test.cpp
  coding_test.cpp
  crc32c_test.cpp
  wal_codec_test.cpp
  posix_file_test.cpp
  posix_dir_test.cpp
  db_test.cpp
  db_recovery_test.cpp
  db_fault_test.cpp
  differential_test.cpp
  file_names_test.cpp
  sst_test.cpp
  manifest_test.cpp
  db_m2_test.cpp
  db_publish_fault_test.cpp
  crash_sim_test.cpp)
target_link_libraries(kv_tests PRIVATE kv GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(kv_tests)
