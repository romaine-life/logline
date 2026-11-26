# The C library (logline.h, logline.c): built and tested from the repository's root
# CMakeLists.txt, and on its own from this folder, as a program's vcpkg port builds it
# (README.md, Using it, C).
cmake_minimum_required(VERSION 3.16)
project(logline VERSION 0.1.0 LANGUAGES C)

option(LOGLINE_TESTS "Build the tests over testdata/golden.json and testdata/redact.json" ON)

add_library(logline logline.c)
add_library(logline::logline ALIAS logline)
set_target_properties(logline PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(logline PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)

# Warnings are errors here, where the library is built and tested; a program building its
# source keeps its own flags.
if(MSVC)
    set(LOGLINE_WARNINGS /W4 /WX)
else()
    set(LOGLINE_WARNINGS -Wall -Wextra -Wpedantic -Werror)
endif()
target_compile_options(logline PRIVATE ${LOGLINE_WARNINGS})

include(GNUInstallDirs)
install(TARGETS logline EXPORT logline-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES logline.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT logline-targets NAMESPACE logline:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/logline)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/logline-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/logline-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/logline)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/logline-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/logline)

if(LOGLINE_TESTS)
    enable_testing()
    add_executable(logline-test logline_test.c)
    set_target_properties(logline-test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(logline-test PRIVATE ${LOGLINE_WARNINGS})
    target_link_libraries(logline-test PRIVATE logline)
    add_test(NAME logline
        COMMAND logline-test ${CMAKE_CURRENT_SOURCE_DIR}/../testdata ${CMAKE_CURRENT_BINARY_DIR}/test-work)
endif()
