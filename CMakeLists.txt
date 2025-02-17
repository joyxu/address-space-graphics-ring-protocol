project(asg)
cmake_minimum_required(VERSION 3.7)
set(CMAKE_CXX_STANDARD 17)

option(OPTION_BUILD_TESTS "Whether to build the tests" TRUE)

if (OPTION_BUILD_TESTS)
    include(gtest.cmake)
endif ()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(ASG_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

if (WIN32)
    set(asg-base-platform-sources base/Thread_win32.cpp)
else()
    set(asg-base-platform-sources base/Thread_pthread.cpp)
endif()

add_library(
    asg-base
    base/ring_buffer.cpp
    base/MessageChannel.cpp
    base/FunctorThread.cpp
    ${asg-base-platform-sources})

target_include_directories(asg-base PUBLIC ${ASG_REPO_ROOT})

add_library(
    asg-client
    client/asg_ring_stream_client.cpp)

target_link_libraries(
    asg-client PUBLIC asg-base)

add_library(
    asg-server
    server/asg_ring_stream_server.cpp)

target_link_libraries(
    asg-server PUBLIC asg-base)

add_executable(asg_unittests tests/asg_unittest.cpp)
target_link_libraries(asg_unittests PRIVATE asg-client asg-server gtest_main)

add_executable(asg_benchmark tests/asg_benchmark.cpp)
target_link_libraries(asg_benchmark PRIVATE asg-client asg-server gtest_main)

add_test(NAME asg_unittests COMMAND asg_unittests)
add_test(NAME asg_benchmark COMMAND asg_benchmark)
