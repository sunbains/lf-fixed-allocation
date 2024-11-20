cmake_minimum_required(VERSION 3.20)
project(lock_free_list VERSION 1.0 LANGUAGES CXX)

set(CMAKE_C_COMPILER /usr/bin/clang)
set(CMAKE_CXX_COMPILER /usr/bin/clang++)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/bin)

include(cmake/CompilerOptions.cmake)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

enable_testing()

# Add Google Test directly
include(FetchContent)

find_package(Threads REQUIRED)

include(cmake/FetchTestAndBenchmark.cmake)

# Create header-only library target (INTERFACE)
add_library(lockfreelist INTERFACE)

target_include_directories(
  lockfreelist
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  lockfreelist
  INTERFACE
    Threads::Threads
)

add_subdirectory(examples)
