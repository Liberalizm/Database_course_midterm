# MiniDB test suite.
#
#   unit/         one executable per engine component, each tested in isolation
#   integration/  the whole engine through the Database API (SQL in, rows out)
#   e2e/          the real shell binary fed with SQL scripts, output compared to golden files
#
# Every test carries CTest labels, so a single layer can be run on its own:
#   ctest -L unit            all unit tests
#   ctest -L buffer_pool     just the buffer pool
#   ctest -L integration
#   ctest -L e2e

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # MSVC: use the same runtime as our code
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

function(minidb_add_test target label)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE minidb_core GTest::gtest_main)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    minidb_set_warnings(${target})
    # One label per test, e.g. "unit:storage:buffer_pool", never a CMake list:
    # gtest_discover_tests in CMake 3.x flattens a list in PROPERTIES, which left
    # every test with just the first label ("unit") in CI. `ctest -L` takes a
    # regex, so -L storage, -L unit and -L buffer_pool all still match.
    string(REPLACE ";" ":" label_text "${label}")
    gtest_discover_tests(${target}
            DISCOVERY_MODE POST_BUILD  # the default, generated at build time for the built configuration
            DISCOVERY_TIMEOUT 60
            PROPERTIES LABELS "${label_text}" TIMEOUT 300)
endfunction()

# ---- unit tests: one binary per component, grouped into two layers ----
set(MINIDB_STORAGE_COMPONENTS
        disk_manager
        lru_replacer
        buffer_pool
        value
        table_page
        table_heap
        bplus_tree
        catalog
        transaction)
set(MINIDB_QUERY_COMPONENTS
        lexer
        parser
        expression
        executors
        planner
        shell)
foreach (component IN LISTS MINIDB_STORAGE_COMPONENTS)
    minidb_add_test(test_${component} "unit;storage;${component}" unit/test_${component}.cpp)
endforeach ()
foreach (component IN LISTS MINIDB_QUERY_COMPONENTS)
    minidb_add_test(test_${component} "unit;query;${component}" unit/test_${component}.cpp)
endforeach ()

# Every unit test file must belong to a layer, otherwise CI would silently skip it.
file(GLOB MINIDB_UNIT_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/unit ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_*.cpp)
foreach (f IN LISTS MINIDB_UNIT_FILES)
    string(REGEX REPLACE "^test_(.*)\\.cpp$" "\\1" c "${f}")
    if (NOT c IN_LIST MINIDB_STORAGE_COMPONENTS AND NOT c IN_LIST MINIDB_QUERY_COMPONENTS)
        message(FATAL_ERROR "tests/unit/${f} is not registered in tests/CMakeLists.txt")
    endif ()
endforeach ()

# ---- integration tests ----
minidb_add_test(test_integration "integration" integration/test_sql.cpp integration/test_persistence.cpp
        integration/test_transactions.cpp)

# ---- end-to-end tests of the shell binary ----
file(GLOB MINIDB_E2E_CASES LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/e2e/*)
foreach (case_dir IN LISTS MINIDB_E2E_CASES)
    if (IS_DIRECTORY ${case_dir})
        get_filename_component(case_name ${case_dir} NAME)
        add_test(NAME e2e.${case_name}
                COMMAND ${CMAKE_COMMAND}
                -DSHELL=$<TARGET_FILE:Database_course_midterm>
                -DCASE_DIR=${case_dir}
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/e2e_work/${case_name}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_case.cmake)
        set_tests_properties(e2e.${case_name} PROPERTIES LABELS "e2e" TIMEOUT 120)
    endif ()
endforeach ()
