cmake_minimum_required(VERSION 3.20)
project(hamt VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(hamt INTERFACE)
add_library(hamt::hamt ALIAS hamt)

target_include_directories(hamt INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(hamt INTERFACE cxx_std_20)

option(HAMT_BUILD_TESTS "Build the hamt test suite" ON)

install(DIRECTORY include/hamt DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS hamt EXPORT hamtTargets)
install(EXPORT hamtTargets
    FILE hamtTargets.cmake
    NAMESPACE hamt::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hamt)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/hamtConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hamtConfig.cmake
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/hamtTargets.cmake\")\n")
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/hamtConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/hamtConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hamt)

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
