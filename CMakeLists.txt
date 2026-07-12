cmake_minimum_required(VERSION 3.14)
project(http-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    set(HTTP_PLATFORM_SOURCE src/platform/socket_windows.cpp)
else()
    set(HTTP_PLATFORM_SOURCE src/platform/socket_linux.cpp)
endif()

find_package(Threads REQUIRED)

# Library
add_library(http STATIC
    http.cpp
    http.h
    src/platform/socket.h
    src/platform/socket_common.cpp
    ${HTTP_PLATFORM_SOURCE}
)
target_include_directories(http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(http PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(http PUBLIC ws2_32)
endif()

# File server library
add_library(fileserver STATIC fileserver.cpp fileserver.h)
target_link_libraries(fileserver PUBLIC http)

# Example
add_executable(http_example example/main.cpp)
target_link_libraries(http_example PRIVATE http fileserver)

# Tests
add_executable(http_tests tests/test_http.cpp)
target_link_libraries(http_tests PRIVATE http)

# FileServer tests
add_executable(fileserver_tests tests/test_fileserver.cpp)
target_link_libraries(fileserver_tests PRIVATE fileserver)
