# Tests scripts/release-preflight.cmake against small fixture repositories.
#   cmake -DSCRIPT=<release-preflight.cmake> -DWORK_DIR=<scratch dir> -P release_preflight_test.cmake

cmake_minimum_required(VERSION 3.20)

if (NOT SCRIPT OR NOT WORK_DIR)
    message(FATAL_ERROR "release_preflight_test.cmake needs SCRIPT and WORK_DIR.")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
set(_failures "")

# Writes a fixture repository whose versions are all <version> except where
# overridden: TAURI <v>, CARGO_LOCK <v>; NOTES none|empty|present.
function(make_fixture dir version)
    cmake_parse_arguments(PARSE_ARGV 2 _arg "" "TAURI;CARGO_LOCK;NOTES" "")
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

list(FILTER _failures EXCLUDE REGEX "^$")
if (_failures)
    list(JOIN _failures "\n  - " _report)
    message(FATAL_ERROR "Release preflight test failed:\n  - ${_report}")
endif()
message(STATUS "Release preflight tests passed.")
