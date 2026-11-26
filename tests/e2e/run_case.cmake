# End-to-end test runner (invoked by CTest via `cmake -P`).
#
# A case is a directory containing steps 1.sql, 2.sql, ... and the expected
# shell output for each step (1.expected, 2.expected, ...). All steps run in
# order against the SAME database file, each in a fresh shell process, so a
# multi-step case also verifies that data persists across restarts.
#
# Required variables: SHELL (path to the shell binary), CASE_DIR, WORK_DIR.

foreach (var SHELL CASE_DIR WORK_DIR)
    if (NOT DEFINED ${var})
        message(FATAL_ERROR "run_case.cmake: ${var} is not set")
    endif ()
endforeach ()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

file(GLOB steps "${CASE_DIR}/*.sql")
list(SORT steps)
if (NOT steps)
    message(FATAL_ERROR "no .sql steps found in ${CASE_DIR}")
endif ()

set(db "${WORK_DIR}/e2e.db")
foreach (sql IN LISTS steps)
    get_filename_component(stem "${sql}" NAME_WE)
    execute_process(
            COMMAND "${SHELL}" "${db}"
            INPUT_FILE "${sql}"
            OUTPUT_VARIABLE actual
            ERROR_VARIABLE err
            RESULT_VARIABLE rc
            TIMEOUT 60)
    if (NOT rc EQUAL 0)
        message(FATAL_ERROR "step ${stem}: shell exited with '${rc}'\nstderr:\n${err}")
    endif ()
    string(REPLACE "\r\n" "\n" actual "${actual}")
    file(WRITE "${WORK_DIR}/${stem}.actual" "${actual}")

    set(expected_file "${CASE_DIR}/${stem}.expected")
    if (NOT EXISTS "${expected_file}")
        message(FATAL_ERROR "step ${stem}: missing ${expected_file}\n"
                "actual output was written to ${WORK_DIR}/${stem}.actual")
    endif ()
    file(READ "${expected_file}" expected)
    string(REPLACE "\r\n" "\n" expected "${expected}")

    if (NOT actual STREQUAL expected)
        message(FATAL_ERROR "step ${stem}: output differs from ${expected_file}\n"
                "---------- expected ----------\n${expected}"
                "----------- actual -----------\n${actual}"
                "------------------------------")
    endif ()
    message(STATUS "step ${stem}: OK")
endforeach ()
