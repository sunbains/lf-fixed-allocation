add_executable(simple
  simple.cc
)

target_include_directories(simple
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(simple
  PRIVATE
    Threads::Threads
)
