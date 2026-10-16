cmake_minimum_required(VERSION 3.25)
project(baker_liveops LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LIVEOPS_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(LIVEOPS_BUILD_TOOLS "Build the publisher (needs a development build of Baker)" ${PROJECT_IS_TOP_LEVEL})

# Baker, as a user takes it: its public headers, at a pinned commit until a release has
# check_layer_text and LayerPolicy (D161). Offline: -DFETCHCONTENT_SOURCE_DIR_BAKER=path.
# A shipping build: -DBAKER_SHIPPING=ON, which Baker reads.
include(FetchContent)
set(BAKER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BAKER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BAKER_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(baker
  GIT_REPOSITORY https://github.com/zach/baker-dev.git
  GIT_TAG 75e1728216a9311d41a4b15c75c65c34d0a99646)
FetchContent_MakeAvailable(baker)

include(cmake/sodium.cmake)

add_subdirectory(client)
add_subdirectory(publish)
if(LIVEOPS_BUILD_TOOLS AND BAKER_SHIPPING)
  message(STATUS "baker-liveops: the publisher's command line needs a development build")
endif()
if(LIVEOPS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
