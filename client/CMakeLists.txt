add_library(baker_liveops_client
  src/manifest.cpp
  src/client.cpp
  ../backends/static/static.cpp)
add_library(liveops::client ALIAS baker_liveops_client)
target_include_directories(baker_liveops_client PUBLIC include ../backends)
target_link_libraries(baker_liveops_client PUBLIC baker::data PRIVATE liveops::sodium)
if(MSVC)
  target_compile_options(baker_liveops_client PRIVATE /W4)
else()
  target_compile_options(baker_liveops_client PRIVATE -Wall -Wextra -Wpedantic)
endif()
