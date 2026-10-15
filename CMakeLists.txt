cmake_minimum_required(VERSION 3.20)
project(DB25SemanticAnalyzer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Supported toolchain: GCC 14+ (the CI baseline). Surface an older GCC as a
# clear, actionable message rather than a deep C++23 template error. A soft
# warning, not a hard error, so other C++23 compilers can still be tried at the
# user's own risk (they are not exercised in CI).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(WARNING
        "DB25 targets GCC 14+ (the supported, CI-tested toolchain); "
        "detected GCC ${CMAKE_CXX_COMPILER_VERSION}, which is older and unsupported.")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The DB25 SQL parser, reached one of two ways.
#
# STANDALONE (this repo's own CI, and anyone building it alone): nothing has
# defined the target, so find it through its installed CMake package export.
# Point CMake at the install prefix with -DCMAKE_PREFIX_PATH=/path/to/parser-install
#
# IN-TREE (a superproject that add_subdirectory's both this repo and the parser):
# the target already exists, and finding a SECOND copy through an install would
# be both wasteful and a chance for the two to be different builds of different
# commits. Reuse what the parent added. This is the same guard db25-physical-plan
# uses for db25logicalplan, and it is what lets a parent add_subdirectory THIS
# repo instead of reaching in and compiling its .cpp files by hand.
#
# Either way the target is named DB25::db25parser - the parser aliases its in-tree
# target to the name its export publishes, so this file does not care which path
# it came from.
if(NOT TARGET DB25::db25parser)
    find_package(DB25Parser REQUIRED)
endif()

add_library(db25semantic
    src/analyzer.cpp
    src/catalog_snapshot.cpp
    src/check_eval.cpp
    src/ddl.cpp
    src/transaction.cpp
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

    add_executable(catalog_snapshot_tests tests/test_catalog_snapshot.cpp)
    target_link_libraries(catalog_snapshot_tests PRIVATE db25semantic DB25::db25parser)
    add_test(NAME catalog_snapshot_tests COMMAND catalog_snapshot_tests)

    add_executable(catalog_manager_tests tests/test_catalog_manager.cpp)
    target_link_libraries(catalog_manager_tests PRIVATE db25semantic DB25::db25parser)
    add_test(NAME catalog_manager_tests COMMAND catalog_manager_tests)

    add_executable(ddl_tests tests/test_ddl.cpp)
    target_link_libraries(ddl_tests PRIVATE db25semantic DB25::db25parser)
    add_test(NAME ddl_tests COMMAND ddl_tests)

    add_executable(transaction_tests tests/test_transaction.cpp)
    target_link_libraries(transaction_tests PRIVATE db25semantic DB25::db25parser)
    add_test(NAME transaction_tests COMMAND transaction_tests)
endif()
