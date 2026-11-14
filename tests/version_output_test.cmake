# Checks the real `arn --version` line: "arn <version> (<12 hex>[-dirty])", or
# "(unknown)" only when the build could not know its commit.
#   cmake -DARN_EXE=<arn> -DEXPECTED_VERSION=<X.Y.Z> -DSOURCE_DIR=<ARN source>
#         [-DGIT_EXECUTABLE=<git>] -P version_output_test.cmake
# When the source is a Git work tree, the commit must be its HEAD; in GitHub
# Actions it must also not be marked dirty.

cmake_minimum_required(VERSION 3.20)

execute_process(COMMAND "${ARN_EXE}" --version
    RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\r" "" _output "${_output}")
if (NOT _result EQUAL 0)
    message(FATAL_ERROR "arn --version failed (${_result}): ${_error}")
endif()

string(REPLACE "." "\\." _version_regex "${EXPECTED_VERSION}")
if (NOT _output MATCHES "^arn ${_version_regex} \\(([0-9a-f]+|unknown)(-dirty)?\\)$")
    message(FATAL_ERROR "Unexpected arn --version output: '${_output}'")
endif()
set(_commit "${CMAKE_MATCH_1}")
set(_dirty "${CMAKE_MATCH_2}")
if (NOT _commit STREQUAL "unknown")
    string(LENGTH "${_commit}" _length)
    if (NOT _length EQUAL 12)
        message(FATAL_ERROR "The commit in '${_output}' does not have 12 digits.")
    endif()
elseif (_dirty)
    message(FATAL_ERROR "'${_output}' marks an unknown commit as dirty.")
endif()

set(_git_head "")
if (GIT_EXECUTABLE AND EXISTS "${GIT_EXECUTABLE}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --show-toplevel
        RESULT_VARIABLE _top_result OUTPUT_VARIABLE _top ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (_top_result EQUAL 0)
        file(REAL_PATH "${_top}" _top)
        file(REAL_PATH "${SOURCE_DIR}" _source)
        if (CMAKE_HOST_WIN32)
            string(TOLOWER "${_top}" _top)
            string(TOLOWER "${_source}" _source)
        endif()
        if (_top STREQUAL _source)
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse HEAD
                OUTPUT_VARIABLE _git_head OUTPUT_STRIP_TRAILING_WHITESPACE)
            string(SUBSTRING "${_git_head}" 0 12 _git_head)
        endif()
    endif()
endif()

if (NOT _git_head STREQUAL "")
    if (NOT _commit STREQUAL _git_head)
        message(FATAL_ERROR "arn --version names ${_commit}, but the source HEAD is ${_git_head}. Rebuild after committing.")
    endif()
    if ("$ENV{GITHUB_ACTIONS}" STREQUAL "true" AND _dirty)
        message(FATAL_ERROR "A GitHub Actions build must not be marked dirty: '${_output}'")
    endif()
endif()
message(STATUS "arn --version: ${_output}")
