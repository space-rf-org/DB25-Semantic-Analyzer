cmake_minimum_required(VERSION 3.20)
project(DB25SemanticAnalyzer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Consume the DB25 SQL parser via its installed CMake package export.
# Point CMake at the install prefix with -DCMAKE_PREFIX_PATH=/path/to/parser-install
find_package(DB25Parser REQUIRED)

add_library(db25semantic
    src/analyzer.cpp
)
target_include_directories(db25semantic PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(db25semantic PUBLIC DB25::db25parser)
target_compile_features(db25semantic PUBLIC cxx_std_23)

option(BUILD_TESTS "Build the semantic analyzer test suite" ON)
if(BUILD_TESTS)
    enable_testing()
    add_executable(semantic_tests tests/test_analyzer.cpp)
    target_link_libraries(semantic_tests PRIVATE db25semantic DB25::db25parser)
    add_test(NAME semantic_tests COMMAND semantic_tests)
endif()
