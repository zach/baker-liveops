# The publisher's parts, which the tests share; the command line only in development builds.
add_library(baker_liveops_publishing game.cpp publish.cpp stack.cpp)
add_library(liveops::publishing ALIAS baker_liveops_publishing)
target_include_directories(baker_liveops_publishing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(baker_liveops_publishing
  PUBLIC liveops::client baker::shape PRIVATE liveops::sodium)

if(LIVEOPS_BUILD_TOOLS AND NOT BAKER_SHIPPING)
  add_executable(baker-liveops main.cpp ../retire/retire.cpp)
  target_link_libraries(baker-liveops PRIVATE liveops::publishing liveops::sodium)
endif()
