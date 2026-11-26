cmake_minimum_required(VERSION 3.16)
project(Database_course_midterm CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/W4)
else ()
    add_compile_options(-Wall -Wextra)
endif ()

# The database engine as a library, shared by the shell and the tests.
add_library(minidb_core STATIC
        src/disk_manager.cpp
        src/buffer_pool.cpp
        src/value.cpp
        src/table_page.cpp
        src/table_heap.cpp
        src/bplus_tree.cpp
        src/catalog.cpp
        src/parser.cpp
        src/expression.cpp
        src/executors.cpp
        src/planner.cpp
        src/database.cpp)
target_include_directories(minidb_core PUBLIC src)

# Interactive SQL shell.
add_executable(Database_course_midterm src/main.cpp)
target_link_libraries(Database_course_midterm PRIVATE minidb_core)

# Tests.
enable_testing()
add_executable(minidb_tests tests/test_main.cpp)
target_link_libraries(minidb_tests PRIVATE minidb_core)
add_test(NAME minidb_tests COMMAND minidb_tests)
