add_executable(liveops_client_test client_test.cpp)
target_link_libraries(liveops_client_test PRIVATE liveops::publishing)
target_compile_definitions(liveops_client_test PRIVATE LIVEOPS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME liveops_client COMMAND liveops_client_test)

add_executable(liveops_bench bench.cpp)
target_link_libraries(liveops_bench PRIVATE liveops::publishing)
target_compile_definitions(liveops_bench PRIVATE LIVEOPS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME liveops_bench_smoke COMMAND liveops_bench 5)
