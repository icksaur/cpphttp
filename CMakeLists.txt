cmake_minimum_required(VERSION 3.14)
project(http-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Library
add_library(http STATIC http.cpp http.h)
target_include_directories(http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# File server library
add_library(fileserver STATIC fileserver.cpp fileserver.h)
target_link_libraries(fileserver PUBLIC http)

# Example
add_executable(http_example example/main.cpp)
target_link_libraries(http_example PRIVATE http fileserver pthread)

# Tests
add_executable(http_tests tests/test_http.cpp)
target_link_libraries(http_tests PRIVATE http pthread)

# FileServer tests
add_executable(fileserver_tests tests/test_fileserver.cpp)
target_link_libraries(fileserver_tests PRIVATE fileserver pthread)
