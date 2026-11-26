# The C library in c/, from the repository's root, where pipeline-templates' cmake suite
# builds it and runs its tests (ctest) on Linux and Windows.
cmake_minimum_required(VERSION 3.16)
project(logline-repository LANGUAGES C)
enable_testing()
add_subdirectory(c)
