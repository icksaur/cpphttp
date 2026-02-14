cmake_minimum_required(VERSION 3.14)
project(http-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Library
add_library(http STATIC http.cpp http.h)
target_include_directories(http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Example
add_executable(http_example example/main.cpp)
target_link_libraries(http_example PRIVATE http pthread)

# Tests
add_executable(http_tests tests/test_http.cpp)
target_link_libraries(http_tests PRIVATE http pthread)
