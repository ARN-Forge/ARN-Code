# Tests scripts/release-checksums.cmake with temporary artifact directories.
#   cmake -DSCRIPT=<release-checksums.cmake> -DWORK_DIR=<scratch dir> -P release_checksums_test.cmake

cmake_minimum_required(VERSION 3.20)

if (NOT SCRIPT OR NOT WORK_DIR)
    message(FATAL_ERROR "release_checksums_test.cmake needs SCRIPT and WORK_DIR.")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
set(_failures "")
set(_expected "b-asset.tar.gz;a-asset.zip;c-setup.exe")

# Writes <dir>/<artifact>/<file> = <content> for each "artifact/file=content".
function(make_artifacts dir)
    foreach(_spec IN LISTS ARGN)
        string(FIND "${_spec}" "=" _eq)
        string(SUBSTRING "${_spec}" 0 ${_eq} _path)
        math(EXPR _start "${_eq} + 1")
        string(SUBSTRING "${_spec}" ${_start} -1 _content)
        file(WRITE "${dir}/${_path}" "${_content}")
    endforeach()
endfunction()

# Runs the script; checks pass/fail, failure message fragments, and that a
# failed run leaves no output directory.
function(expect name artifacts outcome)
    set(_dist "${WORK_DIR}/${name}-dist")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DARTIFACTS_DIR=${artifacts}" "-DDIST_DIR=${_dist}"
                "-DEXPECTED=${_expected}" -P "${SCRIPT}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out} ${_err}")
    set(_found "${_failures}")
    if (outcome STREQUAL "pass")
        if (NOT _result EQUAL 0)
            list(APPEND _found "${name}: expected success, got: ${_text}")
        endif()
    elseif (_result EQUAL 0)
        list(APPEND _found "${name}: expected failure, but it passed")
    else()
        if (EXISTS "${_dist}")
            list(APPEND _found "${name}: a failed run left ${_dist}")
        endif()
        foreach(_fragment IN LISTS ARGN)
            string(FIND "${_text}" "${_fragment}" _at)
            if (_at EQUAL -1)
                list(APPEND _found "${name}: message lacks '${_fragment}': ${_text}")
            endif()
        endforeach()
    endif()
    set(_failures "${_found}" PARENT_SCOPE)
endfunction()

# Valid: three artifacts (one with two files), manifest sorted and exact.
make_artifacts("${WORK_DIR}/ok"
    "release-a/a-asset.zip=zip bytes"
    "release-b/b-asset.tar.gz=tar bytes"
    "release-c/c-setup.exe=setup bytes")
expect(valid "${WORK_DIR}/ok" pass)
set(_dist "${WORK_DIR}/valid-dist")
if (EXISTS "${_dist}/SHA256SUMS")
    file(READ "${_dist}/SHA256SUMS" _manifest)
    set(_wanted "")
    foreach(_name IN ITEMS a-asset.zip b-asset.tar.gz c-setup.exe)
        file(SHA256 "${_dist}/${_name}" _hash)
        string(APPEND _wanted "${_hash}  ${_name}\n")
    endforeach()
    string(SHA256 _independent "zip bytes")
    string(FIND "${_manifest}" "${_independent}  a-asset.zip\n" _at)
    if (NOT _manifest STREQUAL _wanted)
        list(APPEND _failures "valid: manifest is\n${_manifest}\nexpected\n${_wanted}")
    elseif (_at EQUAL -1)
        list(APPEND _failures "valid: a-asset.zip hash differs from the SHA-256 of its content")
    endif()
    string(FIND "${_manifest}" "\r" _cr)
    string(FIND "${_manifest}" "SHA256SUMS" _self)
    string(FIND "${_manifest}" "${WORK_DIR}" _absolute)
    if (NOT _cr EQUAL -1 OR NOT _self EQUAL -1 OR NOT _absolute EQUAL -1)
        list(APPEND _failures "valid: manifest has CR, lists itself, or contains a directory path")
    endif()
    file(GLOB _published RELATIVE "${_dist}" "${_dist}/*")
    list(SORT _published)
    if (NOT _published STREQUAL "SHA256SUMS;a-asset.zip;b-asset.tar.gz;c-setup.exe")
        list(APPEND _failures "valid: published files are ${_published}")
    endif()
else()
    list(APPEND _failures "valid: no SHA256SUMS written")
endif()

make_artifacts("${WORK_DIR}/missing" "release-a/a-asset.zip=x" "release-b/b-asset.tar.gz=y")
expect(missing "${WORK_DIR}/missing" fail "expected asset 'c-setup.exe' is missing")

make_artifacts("${WORK_DIR}/extra" "release-a/a-asset.zip=x" "release-b/b-asset.tar.gz=y"
    "release-c/c-setup.exe=z" "release-c/notes.txt=n")
expect(extra "${WORK_DIR}/extra" fail "unexpected asset 'notes.txt'")

make_artifacts("${WORK_DIR}/duplicate" "release-a/a-asset.zip=x" "release-b/b-asset.tar.gz=y"
    "release-c/c-setup.exe=z" "release-d/a-asset.zip=other")
expect(duplicate "${WORK_DIR}/duplicate" fail "two artifacts contain 'a-asset.zip'")

make_artifacts("${WORK_DIR}/empty" "release-a/a-asset.zip=" "release-b/b-asset.tar.gz=y"
    "release-c/c-setup.exe=z")
expect(empty "${WORK_DIR}/empty" fail "asset 'a-asset.zip' is empty")

make_artifacts("${WORK_DIR}/self" "release-a/a-asset.zip=x" "release-b/b-asset.tar.gz=y"
    "release-c/c-setup.exe=z" "release-c/SHA256SUMS=old")
expect(self "${WORK_DIR}/self" fail "unexpected asset 'SHA256SUMS'")

# Output directory must start empty.
file(MAKE_DIRECTORY "${WORK_DIR}/reuse-dist")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DARTIFACTS_DIR=${WORK_DIR}/ok" "-DDIST_DIR=${WORK_DIR}/reuse-dist"
            "-DEXPECTED=${_expected}" -P "${SCRIPT}"
    RESULT_VARIABLE _reuse ERROR_VARIABLE _reuse_err)
string(REGEX REPLACE "[ \t\r\n]+" " " _reuse_err "${_reuse_err}")
string(FIND "${_reuse_err}" "already exists" _at)
if (_reuse EQUAL 0 OR _at EQUAL -1)
    list(APPEND _failures "existing output directory: accepted or wrong message: ${_reuse_err}")
endif()

# Expected names must be plain file names.
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DARTIFACTS_DIR=${WORK_DIR}/ok" "-DDIST_DIR=${WORK_DIR}/badname-dist"
            "-DEXPECTED=a-asset.zip;../escape.zip" -P "${SCRIPT}"
    RESULT_VARIABLE _badname ERROR_VARIABLE _badname_err)
string(REGEX REPLACE "[ \t\r\n]+" " " _badname_err "${_badname_err}")
string(FIND "${_badname_err}" "is not a plain file name" _at)
if (_badname EQUAL 0 OR _at EQUAL -1)
    list(APPEND _failures "path in expected names: accepted or wrong message: ${_badname_err}")
endif()

# The partial (Linux and macOS) release: exactly its three archives, and a
# Windows package that the build still produced is refused, not published.
set(_expected "arn-linux-x64.tar.gz;arn-macos-arm64.tar.gz;arn-macos-x64.tar.gz")
set(_partial_files
    "release-arn-linux-x64/arn-linux-x64.tar.gz=linux"
    "release-arn-macos-arm64/arn-macos-arm64.tar.gz=arm64"
    "release-arn-macos-x64/arn-macos-x64.tar.gz=intel")
make_artifacts("${WORK_DIR}/partial" ${_partial_files})
expect(partial "${WORK_DIR}/partial" pass)
set(_dist "${WORK_DIR}/partial-dist")
if (EXISTS "${_dist}/SHA256SUMS")
    file(READ "${_dist}/SHA256SUMS" _manifest)
    set(_wanted "")
    foreach(_spec IN ITEMS "arn-linux-x64.tar.gz=linux" "arn-macos-arm64.tar.gz=arm64" "arn-macos-x64.tar.gz=intel")
        string(REPLACE "=" ";" _pair "${_spec}")
        list(GET _pair 0 _name)
        list(GET _pair 1 _content)
        string(SHA256 _hash "${_content}")
        string(APPEND _wanted "${_hash}  ${_name}\n")
    endforeach()
    if (NOT _manifest STREQUAL _wanted)
        list(APPEND _failures "partial: manifest is\n${_manifest}\nexpected\n${_wanted}")
    endif()
    file(GLOB _published RELATIVE "${_dist}" "${_dist}/*")
    list(SORT _published)
    if (NOT _published STREQUAL "SHA256SUMS;arn-linux-x64.tar.gz;arn-macos-arm64.tar.gz;arn-macos-x64.tar.gz")
        list(APPEND _failures "partial: published files are ${_published}")
    endif()
    # An independent implementation must accept the manifest as written.
    find_program(_sha256sum sha256sum)
    find_program(_shasum shasum)
    if (_sha256sum)
        set(_check "${_sha256sum}" --check --strict SHA256SUMS)
    elseif (_shasum)
        set(_check "${_shasum}" -a 256 --check --strict SHA256SUMS)
    else()
        set(_check "")
        message(STATUS "Neither sha256sum nor shasum was found; the independent manifest check is skipped.")
    endif()
    if (_check)
        execute_process(COMMAND ${_check} WORKING_DIRECTORY "${_dist}"
            RESULT_VARIABLE _check_result OUTPUT_VARIABLE _check_out ERROR_VARIABLE _check_err)
        if (NOT _check_result EQUAL 0)
            list(APPEND _failures "partial: ${_check} failed: ${_check_out} ${_check_err}")
        endif()
    endif()
else()
    list(APPEND _failures "partial: no SHA256SUMS written")
endif()

make_artifacts("${WORK_DIR}/partial-windows" ${_partial_files}
    "release-arn-windows-x64/arn-windows-x64.zip=windows")
expect(partial-with-windows-cli "${WORK_DIR}/partial-windows" fail "unexpected asset 'arn-windows-x64.zip'")
make_artifacts("${WORK_DIR}/partial-ide" ${_partial_files}
    "release-arn-ide-windows-x64/arn-ide-windows-x64.zip=ide"
    "release-arn-ide-windows-x64/arn-ide-windows-x64-setup.exe=setup")
expect(partial-with-windows-ide "${WORK_DIR}/partial-ide" fail
    "unexpected asset 'arn-ide-windows-x64.zip'" "unexpected asset 'arn-ide-windows-x64-setup.exe'")
make_artifacts("${WORK_DIR}/partial-no-linux"
    "release-arn-macos-arm64/arn-macos-arm64.tar.gz=arm64"
    "release-arn-macos-x64/arn-macos-x64.tar.gz=intel")
expect(partial-without-linux "${WORK_DIR}/partial-no-linux" fail "expected asset 'arn-linux-x64.tar.gz' is missing")
make_artifacts("${WORK_DIR}/partial-no-arm64"
    "release-arn-linux-x64/arn-linux-x64.tar.gz=linux"
    "release-arn-macos-x64/arn-macos-x64.tar.gz=intel")
expect(partial-without-macos-arm64 "${WORK_DIR}/partial-no-arm64" fail "expected asset 'arn-macos-arm64.tar.gz' is missing")

list(FILTER _failures EXCLUDE REGEX "^$")
if (_failures)
    list(JOIN _failures "\n  - " _report)
    message(FATAL_ERROR "Release checksums test failed:\n  - ${_report}")
endif()
message(STATUS "Release checksums tests passed.")
