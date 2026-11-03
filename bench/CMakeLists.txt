# Benchmarks (docs/m4-design.md section 5). Build them in a Release build
# without sanitizers; they are not part of the test suite.
add_executable(kv_bench kv_bench.cpp)
target_link_libraries(kv_bench PRIVATE kv)
