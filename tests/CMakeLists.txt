add_executable(liveops_client_test client_test.cpp)
target_link_libraries(liveops_client_test PRIVATE liveops::publishing)
target_compile_definitions(liveops_client_test PRIVATE LIVEOPS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME liveops_client COMMAND liveops_client_test)

add_executable(liveops_bench bench.cpp)
target_link_libraries(liveops_bench PRIVATE liveops::publishing)
target_compile_definitions(liveops_bench PRIVATE LIVEOPS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME liveops_bench_smoke COMMAND liveops_bench 5)

add_executable(liveops_host host.cpp)
target_link_libraries(liveops_host PRIVATE liveops::publishing)

# The publisher and the host end to end: development builds, where the publisher is built.
if(TARGET baker-liveops)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME liveops_cli
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py ${PROJECT_BINARY_DIR})
  endif()
endif()
