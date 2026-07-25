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
    src/catalog_snapshot.cpp
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
