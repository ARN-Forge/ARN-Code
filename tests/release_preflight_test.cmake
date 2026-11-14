# Tests scripts/release-preflight.cmake against small fixture repositories.
#   cmake -DSCRIPT=<release-preflight.cmake> -DWORK_DIR=<scratch dir> -P release_preflight_test.cmake

cmake_minimum_required(VERSION 3.20)

if (NOT SCRIPT OR NOT WORK_DIR)
    message(FATAL_ERROR "release_preflight_test.cmake needs SCRIPT and WORK_DIR.")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
set(_failures "")

# The default release platform policy of a fixture: Linux and macOS only, as
# a prerelease.
set(_partial_platforms "\"linux-x64\", \"macos-arm64\", \"macos-x64\"")

# Writes release/platforms.json in <dir>. Arguments override the fields:
# VERSION, PLATFORMS (JSON list contents), PRERELEASE (JSON value), REASON
# (JSON value or "none" to leave it out), FORMAT; RAW replaces the whole file.
function(write_policy dir)
    cmake_parse_arguments(PARSE_ARGV 1 _p "" "VERSION;PLATFORMS;PRERELEASE;REASON;FORMAT;RAW" "")
    if (DEFINED _p_RAW)
        file(WRITE "${dir}/release/platforms.json" "${_p_RAW}")
        return()
    endif()
    if (NOT DEFINED _p_PLATFORMS)
        set(_p_PLATFORMS "${_partial_platforms}")
    endif()
    if (NOT DEFINED _p_PRERELEASE)
        set(_p_PRERELEASE true)
    endif()
    if (NOT DEFINED _p_REASON)
        set(_p_REASON "\"Windows packages are withheld.\"")
    endif()
    if (NOT DEFINED _p_FORMAT)
        set(_p_FORMAT "arn-release-platforms/1")
    endif()
    set(_reason_line ",\n  \"reason\": ${_p_REASON}")
    if (_p_REASON STREQUAL "none")
        set(_reason_line "")
    endif()
    file(WRITE "${dir}/release/platforms.json"
        "{\n  \"format\": \"${_p_FORMAT}\",\n  \"version\": \"${_p_VERSION}\",\n"
        "  \"platforms\": [${_p_PLATFORMS}],\n  \"prerelease\": ${_p_PRERELEASE}${_reason_line}\n}\n")
endfunction()

# Writes a fixture repository whose versions are all <version> except where
# overridden: TAURI <v>, CARGO_LOCK <v>; NOTES none|empty|present. Its
# platform policy is the partial default for <version>.
function(make_fixture dir version)
    cmake_parse_arguments(PARSE_ARGV 2 _arg "" "TAURI;CARGO_LOCK;NOTES" "")
    write_policy("${dir}" VERSION "${version}")
    if (NOT _arg_TAURI)
        set(_arg_TAURI "${version}")
    endif()
    if (NOT _arg_CARGO_LOCK)
        set(_arg_CARGO_LOCK "${version}")
    endif()
    if (NOT _arg_NOTES)
        set(_arg_NOTES present)
    endif()
    file(WRITE "${dir}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.20)\n\nproject(Fixture VERSION ${version} LANGUAGES CXX)\n")
    file(WRITE "${dir}/ide/package.json" "{\n  \"name\": \"arn-ide\",\n  \"version\": \"${version}\"\n}\n")
    file(WRITE "${dir}/ide/package-lock.json"
        "{\n  \"name\": \"arn-ide\",\n  \"version\": \"${version}\",\n  \"lockfileVersion\": 3,\n"
        "  \"packages\": {\n    \"\": {\n      \"name\": \"arn-ide\",\n      \"version\": \"${version}\"\n    }\n  }\n}\n")
    file(WRITE "${dir}/ide/src-tauri/tauri.conf.json"
        "{\n  \"productName\": \"ARN IDE\",\n  \"version\": \"${_arg_TAURI}\"\n}\n")
    file(WRITE "${dir}/ide/src-tauri/Cargo.toml"
        "[package]\nname = \"arn-ide\"\nversion = \"${version}\"\nedition = \"2021\"\n\n[dependencies]\nserde = { version = \"1\" }\n")
    file(WRITE "${dir}/ide/src-tauri/Cargo.lock"
        "version = 4\n\n[[package]]\nname = \"arn-ide\"\nversion = \"${_arg_CARGO_LOCK}\"\n")
    if (_arg_NOTES STREQUAL "present")
        file(WRITE "${dir}/docs/releases/v${version}.md" "# ARN Code v${version}\n")
    elseif (_arg_NOTES STREQUAL "empty")
        file(WRITE "${dir}/docs/releases/v${version}.md" "  \n")
    endif()
endfunction()

# Runs the preflight; checks pass/fail and, for failures, a message fragment.
function(expect name dir ref_type ref_name outcome)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DREPO_ROOT=${dir}" "-DREF_TYPE=${ref_type}"
                "-DREF_NAME=${ref_name}" -P "${SCRIPT}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out} ${_err}")
    set(_found "${_failures}")
    if (outcome STREQUAL "pass")
        if (NOT _result EQUAL 0)
            list(APPEND _found "${name}: expected success, got: ${_text}")
        endif()
    elseif (_result EQUAL 0)
        list(APPEND _found "${name}: expected failure, but the preflight passed")
    else()
        foreach(_fragment IN LISTS ARGN)
            string(FIND "${_text}" "${_fragment}" _at)
            if (_at EQUAL -1)
                list(APPEND _found "${name}: message lacks '${_fragment}': ${_text}")
            endif()
        endforeach()
    endif()
    set(_failures "${_found}" PARENT_SCOPE)
endfunction()

make_fixture("${WORK_DIR}/ok" 1.2.3)
expect("matching tag" "${WORK_DIR}/ok" tag v1.2.3 pass)

# Manual runs on a branch, and tags that are not vX.Y.Z.
expect("manual run on main" "${WORK_DIR}/ok" branch main fail "only from a vX.Y.Z tag" "branch 'main'")
expect("empty ref type" "${WORK_DIR}/ok" "" v1.2.3 fail "only from a vX.Y.Z tag")
expect("tag named main" "${WORK_DIR}/ok" tag main fail "'main' is not of the form vX.Y.Z")
expect("pre-release tag" "${WORK_DIR}/ok" tag v1.2.3-rc.1 fail "is not of the form vX.Y.Z")
expect("tag without v" "${WORK_DIR}/ok" tag 1.2.3 fail "is not of the form vX.Y.Z")
expect("leading zero" "${WORK_DIR}/ok" tag v01.2.3 fail "is not of the form vX.Y.Z")

# The tag must match every version.
expect("tag ahead of the project" "${WORK_DIR}/ok" tag v1.2.4 fail
    "CMakeLists.txt project(VERSION) is 1.2.3, but the tag is v1.2.4"
    "ide/package.json version is 1.2.3"
    "Release notes docs/releases/v1.2.4.md are missing")
make_fixture("${WORK_DIR}/tauri" 1.2.3 TAURI 1.2.2)
expect("IDE version mismatch" "${WORK_DIR}/tauri" tag v1.2.3 fail
    "ide/src-tauri/tauri.conf.json version is 1.2.2, but the tag is v1.2.3")
make_fixture("${WORK_DIR}/lock" 1.2.3 CARGO_LOCK 1.2.2)
expect("Cargo.lock mismatch" "${WORK_DIR}/lock" tag v1.2.3 fail
    "ide/src-tauri/Cargo.lock arn-ide version is 1.2.2")

# Release notes.
make_fixture("${WORK_DIR}/no-notes" 1.2.3 NOTES none)
expect("missing release notes" "${WORK_DIR}/no-notes" tag v1.2.3 fail
    "Release notes docs/releases/v1.2.3.md are missing")
make_fixture("${WORK_DIR}/empty-notes" 1.2.3 NOTES empty)
expect("empty release notes" "${WORK_DIR}/empty-notes" tag v1.2.3 fail
    "Release notes docs/releases/v1.2.3.md are empty")

# Missing version file.
make_fixture("${WORK_DIR}/no-tauri" 1.2.3)
file(REMOVE "${WORK_DIR}/no-tauri/ide/src-tauri/tauri.conf.json")
expect("missing tauri.conf.json" "${WORK_DIR}/no-tauri" tag v1.2.3 fail
    "ide/src-tauri/tauri.conf.json is missing")

# The release platform policy.
# outputs <name> <dir> <tag> <expected key=value lines...>: runs the preflight
# with OUTPUT_FILE; it must pass and write exactly these lines. A "|" in a
# line stands for ";", which would split a CMake list argument.
function(expect_outputs name dir tag)
    set(_file "${WORK_DIR}/${name}.out")
    file(REMOVE "${_file}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DREPO_ROOT=${dir}" "-DREF_TYPE=tag" "-DREF_NAME=${tag}"
                "-DOUTPUT_FILE=${_file}" -P "${SCRIPT}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_found "${_failures}")
    if (NOT _result EQUAL 0)
        string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out} ${_err}")
        list(APPEND _found "${name}: expected success, got: ${_text}")
    elseif (NOT EXISTS "${_file}")
        list(APPEND _found "${name}: no outputs written")
    else()
        file(READ "${_file}" _written)
        set(_wanted "")
        foreach(_line IN LISTS ARGN)
            string(REPLACE "|" ";" _line "${_line}")
            string(APPEND _wanted "${_line}\n")
        endforeach()
        if (NOT _written STREQUAL _wanted)
            list(APPEND _found "${name}: outputs are\n${_written}\nexpected\n${_wanted}")
        endif()
    endif()
    set(_failures "${_found}" PARENT_SCOPE)
endfunction()

set(_linux_job "{\"name\":\"Linux x64\",\"os\":\"ubuntu-24.04\",\"artifact\":\"arn-linux-x64\",\"archive\":\"arn-linux-x64.tar.gz\"}")
set(_arm_job "{\"name\":\"macOS arm64\",\"os\":\"macos-latest\",\"artifact\":\"arn-macos-arm64\",\"archive\":\"arn-macos-arm64.tar.gz\"}")
set(_intel_job "{\"name\":\"macOS x64\",\"os\":\"macos-15-intel\",\"artifact\":\"arn-macos-x64\",\"archive\":\"arn-macos-x64.tar.gz\"}")
set(_windows_job "{\"name\":\"Windows x64\",\"os\":\"windows-latest\",\"artifact\":\"arn-windows-x64\",\"archive\":\"arn-windows-x64.zip\"}")

# Partial (Linux and macOS): exactly three assets, no Windows job, a
# prerelease that is not made the latest release.
expect_outputs("partial policy" "${WORK_DIR}/ok" v1.2.3
    "build_matrix={\"include\":[${_linux_job},${_arm_job},${_intel_job}]}"
    "windows_ide=false"
    "expected=arn-linux-x64.tar.gz|arn-macos-arm64.tar.gz|arn-macos-x64.tar.gz"
    "prerelease=true"
    "make_latest=false"
    "platforms=linux-x64,macos-arm64,macos-x64")

# Complete: every package, the latest release.
make_fixture("${WORK_DIR}/full" 1.2.3)
write_policy("${WORK_DIR}/full" VERSION 1.2.3 PRERELEASE false REASON none
    PLATFORMS "\"windows-x64\", \"linux-x64\", \"macos-arm64\", \"macos-x64\", \"windows-ide-x64\"")
expect_outputs("complete policy" "${WORK_DIR}/full" v1.2.3
    "build_matrix={\"include\":[${_windows_job},${_linux_job},${_arm_job},${_intel_job}]}"
    "windows_ide=true"
    "expected=arn-windows-x64.zip|arn-linux-x64.tar.gz|arn-macos-arm64.tar.gz|arn-macos-x64.tar.gz|arn-ide-windows-x64.zip|arn-ide-windows-x64-setup.exe"
    "prerelease=false"
    "make_latest=true"
    "platforms=windows-x64,linux-x64,macos-arm64,macos-x64,windows-ide-x64")

# The same complete set as a prerelease is allowed, and not made latest.
make_fixture("${WORK_DIR}/full-pre" 1.2.3)
write_policy("${WORK_DIR}/full-pre" VERSION 1.2.3 PRERELEASE true REASON none
    PLATFORMS "\"linux-x64\", \"windows-x64\", \"windows-ide-x64\", \"macos-x64\", \"macos-arm64\"")
expect_outputs("complete prerelease, listed in another order" "${WORK_DIR}/full-pre" v1.2.3
    "build_matrix={\"include\":[${_windows_job},${_linux_job},${_arm_job},${_intel_job}]}"
    "windows_ide=true"
    "expected=arn-windows-x64.zip|arn-linux-x64.tar.gz|arn-macos-arm64.tar.gz|arn-macos-x64.tar.gz|arn-ide-windows-x64.zip|arn-ide-windows-x64-setup.exe"
    "prerelease=true"
    "make_latest=false"
    "platforms=linux-x64,windows-x64,windows-ide-x64,macos-x64,macos-arm64")

# A policy that is wrong for this release fails the preflight, so nothing is
# built or published.
function(policy_case name)
    cmake_parse_arguments(PARSE_ARGV 1 _c "" "" "POLICY;MESSAGES")
    make_fixture("${WORK_DIR}/${name}" 1.2.3)
    write_policy("${WORK_DIR}/${name}" ${_c_POLICY})
    expect("${name}" "${WORK_DIR}/${name}" tag v1.2.3 fail ${_c_MESSAGES})
    set(_failures "${_failures}" PARENT_SCOPE)
endfunction()
policy_case(partial-as-stable POLICY VERSION 1.2.3 PRERELEASE false
    MESSAGES "leaves out windows-x64, windows-ide-x64, so the release is incomplete and must have \"prerelease\": true")
policy_case(stale-policy POLICY VERSION 1.2.2
    MESSAGES "release/platforms.json is the policy for 1.2.2, but the tag is v1.2.3")
policy_case(unknown-platform POLICY VERSION 1.2.3 PLATFORMS "\"linux-x64\", \"windows-arm64\""
    MESSAGES "lists 'windows-arm64', which is not one of")
policy_case(duplicate-platform POLICY VERSION 1.2.3 PLATFORMS "\"linux-x64\", \"linux-x64\""
    MESSAGES "lists 'linux-x64' twice")
policy_case(no-platforms POLICY RAW "{\"format\": \"arn-release-platforms/1\", \"version\": \"1.2.3\", \"platforms\": [], \"prerelease\": true, \"reason\": \"x\"}"
    MESSAGES "lists no platforms")
policy_case(ide-only POLICY VERSION 1.2.3 PLATFORMS "\"windows-ide-x64\""
    MESSAGES "lists no CLI platform")
policy_case(platform-not-a-string POLICY VERSION 1.2.3 PLATFORMS "\"linux-x64\", 7"
    MESSAGES "lists '7', which is not one of")
policy_case(no-reason POLICY VERSION 1.2.3 REASON none
    MESSAGES "but gives no \"reason\"")
policy_case(blank-reason POLICY VERSION 1.2.3 REASON "\"  \""
    MESSAGES "but gives no \"reason\"")
policy_case(prerelease-as-text POLICY VERSION 1.2.3 PRERELEASE "\"true\""
    MESSAGES "must have \"prerelease\": true or false")
policy_case(wrong-format POLICY VERSION 1.2.3 FORMAT "arn-release-platforms/2"
    MESSAGES "must have \"format\": \"arn-release-platforms/1\"")
policy_case(not-an-object POLICY RAW "[\"linux-x64\"]"
    MESSAGES "release/platforms.json is not a JSON object")
policy_case(not-json POLICY RAW "linux-x64, macos-arm64"
    MESSAGES "release/platforms.json is not a JSON object")
make_fixture("${WORK_DIR}/no-policy" 1.2.3)
file(REMOVE "${WORK_DIR}/no-policy/release/platforms.json")
expect("missing policy" "${WORK_DIR}/no-policy" tag v1.2.3 fail "release/platforms.json is missing")

# A failed preflight writes no outputs.
file(REMOVE "${WORK_DIR}/failed.out")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DREPO_ROOT=${WORK_DIR}/partial-as-stable" "-DREF_TYPE=tag" "-DREF_NAME=v1.2.3"
            "-DOUTPUT_FILE=${WORK_DIR}/failed.out" -P "${SCRIPT}"
    RESULT_VARIABLE _failed_result OUTPUT_QUIET ERROR_QUIET)
if (_failed_result EQUAL 0 OR EXISTS "${WORK_DIR}/failed.out")
    list(APPEND _failures "failed preflight: it passed or wrote outputs")
endif()

list(FILTER _failures EXCLUDE REGEX "^$")
if (_failures)
    list(JOIN _failures "\n  - " _report)
    message(FATAL_ERROR "Release preflight test failed:\n  - ${_report}")
endif()
message(STATUS "Release preflight tests passed.")
