message(STATUS "Building tests")

enable_testing()

SET(LIBS gtest_main)
SET(INCLUDE_DIRS  ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/include)

# Create test executable
add_executable(simple_tests simple_tests.cc)
add_executable(mt_tests mt_tests.cc)

# Link against our library and GTest
target_link_libraries(simple_tests PRIVATE ${LIBS})
target_link_libraries(mt_tests PRIVATE ${LIBS})

# Include test directory
target_include_directories(simple_tests PRIVATE ${INCLUDE_DIRS})
target_include_directories(mt_tests PRIVATE ${INCLUDE_DIRS})

# Discover tests
include(GoogleTest)

gtest_discover_tests(simple_tests)
gtest_discover_tests(mt_tests)

# Add custom target for running tests with detailed output
add_custom_target(
  test_verbose
  COMMAND ${CMAKE_CTEST_COMMAND} --verbose
  DEPENDS
    simple_tests
    mt_tests)

# Benchmark tests
add_executable(benchmark-1 benchmark-1.cc)
target_include_directories(benchmark-1 PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(benchmark-1 PRIVATE benchmark::benchmark)

add_executable(benchmark-2 benchmark-2.cc)
target_include_directories(benchmark-2 PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(benchmark-2 PRIVATE benchmark::benchmark)
