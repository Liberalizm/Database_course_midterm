cmake_minimum_required(VERSION 3.16)
project(Database_course_midterm CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(MINIDB_BUILD_TESTS "Build the MiniDB test suite" ON)
option(MINIDB_WERROR "Treat compiler warnings in MiniDB code as errors" OFF)
option(MINIDB_SANITIZE "Build everything with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

if (MINIDB_SANITIZE)
    if (MSVC)
        message(FATAL_ERROR "MINIDB_SANITIZE is only supported with GCC/Clang")
    endif ()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif ()

# Warning flags for our own targets only (not for third-party code such as GoogleTest).
function(minidb_set_warnings target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if (MINIDB_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif ()
    else ()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        if (MINIDB_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif ()
    endif ()
endfunction()

# The database engine as a library, shared by the shell and the tests.
add_library(minidb_core STATIC
        src/file_io.cpp
        src/wal.cpp
        src/disk_manager.cpp
        src/buffer_pool.cpp
        src/value.cpp
        src/table_page.cpp
        src/table_heap.cpp
        src/bplus_tree.cpp
        src/catalog.cpp
        src/statistics.cpp
        src/parser.cpp
        src/expression.cpp
        src/executors.cpp
        src/planner.cpp
        src/write_set.cpp
        src/database.cpp
        src/shell.cpp)
target_include_directories(minidb_core PUBLIC src)
find_package(Threads REQUIRED)  # Connections may be used from several threads
target_link_libraries(minidb_core PUBLIC Threads::Threads)
minidb_set_warnings(minidb_core)

# Interactive SQL shell.
add_executable(Database_course_midterm src/main.cpp)
target_link_libraries(Database_course_midterm PRIVATE minidb_core)
minidb_set_warnings(Database_course_midterm)

if (MINIDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()
