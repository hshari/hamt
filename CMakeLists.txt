cmake_minimum_required(VERSION 3.20)
project(hamt VERSION 0.1.0 LANGUAGES CXX)

add_library(hamt INTERFACE)
add_library(hamt::hamt ALIAS hamt)

target_include_directories(hamt INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(hamt INTERFACE cxx_std_20)

option(HAMT_BUILD_TESTS "Build the hamt test suite" ON)

if(HAMT_BUILD_TESTS)
    enable_testing()
    add_executable(hamt_tests tests/test_hamt.cpp)
    target_link_libraries(hamt_tests PRIVATE hamt)
    if(MSVC)
        target_compile_options(hamt_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(hamt_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME hamt_tests COMMAND hamt_tests)
endif()
