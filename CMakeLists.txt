cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
execute_process(COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/source_identity.py"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE MEMO_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    RESULT_VARIABLE MEMO_IDENTITY_RESULT)
if(NOT MEMO_REV)
    message(FATAL_ERROR "Source identity unavailable; run scripts/bootstrap.sh first")
endif()
set(PROJECT_VER "0.1.0-${MEMO_REV}")
project(meow_inbox)
