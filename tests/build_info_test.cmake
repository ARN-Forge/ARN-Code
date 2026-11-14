# Tests cmake/ArnBuildInfo.cmake, which writes the commit shown by `arn --version`.
#   cmake -DSCRIPT=<ArnBuildInfo.cmake> -DWORK_DIR=<empty scratch dir>
#         [-DGIT_EXECUTABLE=<git>] -P build_info_test.cmake
# Git cases are skipped (and reported) when Git is not available.

cmake_minimum_required(VERSION 3.20)

if (NOT SCRIPT OR NOT WORK_DIR)
    message(FATAL_ERROR "build_info_test.cmake needs SCRIPT and WORK_DIR.")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/plain")
set(_failures "")
set(_header "${WORK_DIR}/arn_build_info.hpp")
set(_sha "0123456789abcdef0123456789abcdef01234567")

# Runs the script with GITHUB_ACTIONS/GITHUB_SHA removed unless given in ENV.
# Sets <prefix>_RESULT and <prefix>_COMMIT (the ARN_BUILD_COMMIT value).
function(run_info prefix source git explicit)
    cmake_parse_arguments(PARSE_ARGV 4 _arg "" "" "ENV")
    file(REMOVE "${_header}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=GITHUB_ACTIONS --unset=GITHUB_SHA ${_arg_ENV}
                "${CMAKE_COMMAND}" "-DSOURCE_DIR=${source}" "-DOUTPUT=${_header}"
                "-DGIT_EXECUTABLE=${git}" "-DEXPLICIT_COMMIT=${explicit}" -P "${SCRIPT}"
        RESULT_VARIABLE _result OUTPUT_QUIET ERROR_QUIET)
    set(_commit "")
    if (EXISTS "${_header}")
        file(READ "${_header}" _text)
        if (_text MATCHES "#define ARN_BUILD_COMMIT \"([^\"]*)\"")
            set(_commit "${CMAKE_MATCH_1}")
        endif()
    endif()
    set(${prefix}_RESULT "${_result}" PARENT_SCOPE)
    set(${prefix}_COMMIT "${_commit}" PARENT_SCOPE)
endfunction()

macro(expect_commit name prefix expected)
    if (NOT ${prefix}_RESULT EQUAL 0 OR NOT "${${prefix}_COMMIT}" STREQUAL "${expected}")
        list(APPEND _failures "${name}: expected '${expected}', got '${${prefix}_COMMIT}' (exit ${${prefix}_RESULT})")
    endif()
endmacro()

macro(expect_failure name prefix)
    if (${prefix}_RESULT EQUAL 0)
        list(APPEND _failures "${name}: the script accepted invalid input")
    endif()
endmacro()

# Without Git and outside GitHub Actions the commit is unknown; the build must not fail.
run_info(no_git "${WORK_DIR}/plain" "" "")
expect_commit("no Git" no_git "unknown")
run_info(missing_git "${WORK_DIR}/plain" "${WORK_DIR}/no-such-git" "")
expect_commit("Git path that does not exist" missing_git "unknown")

# GitHub Actions without Git: GITHUB_SHA is used, never marked dirty.
run_info(gha "${WORK_DIR}/plain" "" "" ENV GITHUB_ACTIONS=true GITHUB_SHA=${_sha})
expect_commit("GitHub Actions without Git" gha "0123456789ab")
run_info(gha_bad "${WORK_DIR}/plain" "" "" ENV GITHUB_ACTIONS=true GITHUB_SHA=not-a-sha)
expect_commit("GitHub Actions with an invalid GITHUB_SHA" gha_bad "unknown")

# An explicit commit wins and is validated.
run_info(explicit "${WORK_DIR}/plain" "" "${_sha}")
expect_commit("explicit commit" explicit "0123456789ab")
run_info(explicit_upper "${WORK_DIR}/plain" "" "0123456789ABCDEF")
expect_commit("explicit upper-case commit" explicit_upper "0123456789ab")
run_info(explicit_bad "${WORK_DIR}/plain" "" "not-a-sha")
expect_failure("explicit non-hex commit" explicit_bad)
run_info(explicit_short "${WORK_DIR}/plain" "" "abc123")
expect_failure("explicit commit shorter than 12 digits" explicit_short)

# The header is rewritten only when its content changes.
file(REMOVE "${_header}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=GITHUB_ACTIONS --unset=GITHUB_SHA
    "${CMAKE_COMMAND}" "-DSOURCE_DIR=${WORK_DIR}/plain" "-DOUTPUT=${_header}" -P "${SCRIPT}")
file(TIMESTAMP "${_header}" _first "%Y%m%d%H%M%S")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.2)
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=GITHUB_ACTIONS --unset=GITHUB_SHA
    "${CMAKE_COMMAND}" "-DSOURCE_DIR=${WORK_DIR}/plain" "-DOUTPUT=${_header}" -P "${SCRIPT}")
file(TIMESTAMP "${_header}" _second "%Y%m%d%H%M%S")
if (NOT _first STREQUAL _second)
    list(APPEND _failures "unchanged identity: the header was rewritten (${_first} -> ${_second})")
endif()

if (GIT_EXECUTABLE AND EXISTS "${GIT_EXECUTABLE}")
    set(_repo "${WORK_DIR}/repo")
    file(MAKE_DIRECTORY "${_repo}/sub")
    set(_git "${GIT_EXECUTABLE}" -C "${_repo}" -c core.autocrlf=false)
    execute_process(COMMAND ${_git} init -q RESULT_VARIABLE _init)
    file(WRITE "${_repo}/tracked.txt" "one\n")
    file(WRITE "${_repo}/sub/file.txt" "sub\n")
    execute_process(COMMAND ${_git} add tracked.txt sub/file.txt)
    execute_process(COMMAND ${_git} -c user.name=ARN -c user.email=arn@example.invalid
                    -c commit.gpgsign=false commit -q -m init RESULT_VARIABLE _commit_result)
    execute_process(COMMAND ${_git} rev-parse HEAD OUTPUT_VARIABLE _head
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(SUBSTRING "${_head}" 0 12 _head12)
    if (NOT _init EQUAL 0 OR NOT _commit_result EQUAL 0 OR NOT _head MATCHES "^[0-9a-f]+$")
        list(APPEND _failures "could not create the test Git repository")
    else()
        run_info(clean "${_repo}" "${GIT_EXECUTABLE}" "")
        expect_commit("clean Git work tree" clean "${_head12}")

        file(WRITE "${_repo}/untracked.txt" "new\n")
        run_info(untracked "${_repo}" "${GIT_EXECUTABLE}" "")
        expect_commit("untracked file only" untracked "${_head12}")

        file(WRITE "${_repo}/tracked.txt" "two\n")
        run_info(dirty "${_repo}" "${GIT_EXECUTABLE}" "")
        expect_commit("modified tracked file" dirty "${_head12}-dirty")

        run_info(dirty_gha "${_repo}" "${GIT_EXECUTABLE}" "" ENV GITHUB_ACTIONS=true GITHUB_SHA=${_sha})
        expect_commit("modified file in GitHub Actions" dirty_gha "${_head12}")

        run_info(override "${_repo}" "${GIT_EXECUTABLE}" "${_sha}")
        expect_commit("explicit commit over Git" override "0123456789ab")

        # A source tree inside another repository must not take its commit.
        run_info(nested "${_repo}/sub" "${GIT_EXECUTABLE}" "")
        expect_commit("directory inside another repository" nested "unknown")
    endif()
else()
    message(STATUS "Git not available: Git cases skipped")
endif()

if (_failures)
    list(JOIN _failures "\n  - " _report)
    message(FATAL_ERROR "Build identity test failed:\n  - ${_report}")
endif()
message(STATUS "Build identity tests passed.")
