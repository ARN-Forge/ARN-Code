# Release preflight: checks that a release run is allowed before anything is
# built. Run by .github/workflows/release.yml and by tests/release_preflight_test.cmake.
#
#   cmake -DREF_TYPE=<tag|branch> -DREF_NAME=<name> [-DREPO_ROOT=<dir>]
#         -P scripts/release-preflight.cmake
#
# Requirements:
#   - the run is for a Git tag (a manual run on a branch such as main must not
#     publish a release named after the branch);
#   - the tag is vX.Y.Z;
#   - X.Y.Z equals project(VERSION) in CMakeLists.txt and the IDE versions in
#     ide/package.json, ide/package-lock.json, ide/src-tauri/tauri.conf.json,
#     ide/src-tauri/Cargo.toml and ide/src-tauri/Cargo.lock;
#   - docs/releases/vX.Y.Z.md exists and is not empty.
# Every problem is reported; the script then fails once.

cmake_minimum_required(VERSION 3.20)

if (NOT REPO_ROOT)
    get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(_errors "")
macro(_preflight_error text)
    list(APPEND _errors "${text}")
endmacro()

# Reads a file, or records an error and leaves the variable empty.
function(_preflight_read relative out_var)
    set(_path "${REPO_ROOT}/${relative}")
    if (EXISTS "${_path}")
        file(READ "${_path}" _content)
        set(${out_var} "${_content}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
        set(_errors "${_errors};${relative} is missing." PARENT_SCOPE)
    endif()
endfunction()

# 1. Only tag runs may publish.
if (NOT REF_TYPE STREQUAL "tag")
    message(FATAL_ERROR
        "Release preflight failed:\n"
        "  - Releases are published only from a vX.Y.Z tag; this run is for ${REF_TYPE} '${REF_NAME}'. "
        "Push the tag, or start the workflow on the tag instead of a branch.")
endif()

# 2. Tag format.
set(_version "")
if (REF_NAME MATCHES "^v((0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*))$")
    set(_version "${CMAKE_MATCH_1}")
else()
    message(FATAL_ERROR
        "Release preflight failed:\n"
        "  - Tag '${REF_NAME}' is not of the form vX.Y.Z (for example v1.0.0).")
endif()

# 3. CLI version.
_preflight_read("CMakeLists.txt" _cmake)
if (_cmake MATCHES "project\\([^)]*VERSION[ \t\r\n]+([0-9]+\\.[0-9]+\\.[0-9]+)")
    if (NOT CMAKE_MATCH_1 STREQUAL _version)
        _preflight_error("CMakeLists.txt project(VERSION) is ${CMAKE_MATCH_1}, but the tag is ${REF_NAME}.")
    endif()
elseif (NOT _cmake STREQUAL "")
    _preflight_error("CMakeLists.txt has no project(... VERSION X.Y.Z ...).")
endif()

# 4. IDE versions.
function(_preflight_json relative)
    _preflight_read("${relative}" _json)
    if (_json STREQUAL "")
        set(_errors "${_errors}" PARENT_SCOPE)
        return()
    endif()
    foreach(_path IN LISTS ARGN)
        string(REPLACE "." ";" _keys "${_path}")
        string(JSON _value ERROR_VARIABLE _json_error GET "${_json}" ${_keys})
        if (_json_error)
            set(_errors "${_errors};${relative} has no '${_path}' version field." PARENT_SCOPE)
            return()
        endif()
        if (NOT _value STREQUAL _version)
            set(_errors "${_errors};${relative} ${_path} is ${_value}, but the tag is ${REF_NAME}." PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(_errors "${_errors}" PARENT_SCOPE)
endfunction()
_preflight_json("ide/package.json" "version")
_preflight_json("ide/src-tauri/tauri.conf.json" "version")
# package-lock.json keeps the root version twice: at the top and for packages."".
_preflight_read("ide/package-lock.json" _lock)
if (NOT _lock STREQUAL "")
    string(JSON _lock_version ERROR_VARIABLE _lock_error GET "${_lock}" version)
    string(JSON _lock_root ERROR_VARIABLE _lock_root_error GET "${_lock}" packages "" version)
    if (_lock_error OR _lock_root_error)
        _preflight_error("ide/package-lock.json has no root version.")
    elseif (NOT _lock_version STREQUAL _version OR NOT _lock_root STREQUAL _version)
        _preflight_error("ide/package-lock.json root version is ${_lock_version}/${_lock_root}, but the tag is ${REF_NAME}.")
    endif()
endif()

_preflight_read("ide/src-tauri/Cargo.toml" _cargo)
if (_cargo MATCHES "\\[package\\][^[]*\nversion[ \t]*=[ \t]*\"([^\"]+)\"")
    if (NOT CMAKE_MATCH_1 STREQUAL _version)
        _preflight_error("ide/src-tauri/Cargo.toml [package] version is ${CMAKE_MATCH_1}, but the tag is ${REF_NAME}.")
    endif()
elseif (NOT _cargo STREQUAL "")
    _preflight_error("ide/src-tauri/Cargo.toml has no [package] version.")
endif()

_preflight_read("ide/src-tauri/Cargo.lock" _cargo_lock)
if (_cargo_lock MATCHES "name = \"arn-ide\"\r?\nversion = \"([^\"]+)\"")
    if (NOT CMAKE_MATCH_1 STREQUAL _version)
        _preflight_error("ide/src-tauri/Cargo.lock arn-ide version is ${CMAKE_MATCH_1}, but the tag is ${REF_NAME}.")
    endif()
elseif (NOT _cargo_lock STREQUAL "")
    _preflight_error("ide/src-tauri/Cargo.lock has no arn-ide package.")
endif()

# 5. Release notes.
set(_notes "${REPO_ROOT}/docs/releases/${REF_NAME}.md")
if (NOT EXISTS "${_notes}")
    _preflight_error("Release notes docs/releases/${REF_NAME}.md are missing.")
else()
    file(READ "${_notes}" _notes_text)
    string(STRIP "${_notes_text}" _notes_text)
    if (_notes_text STREQUAL "")
        _preflight_error("Release notes docs/releases/${REF_NAME}.md are empty.")
    endif()
endif()

list(FILTER _errors EXCLUDE REGEX "^$")
if (_errors)
    set(_report "Release preflight failed for ${REF_NAME}:")
    foreach(_error IN LISTS _errors)
        string(APPEND _report "\n  - ${_error}")
    endforeach()
    message(FATAL_ERROR "${_report}")
endif()
message(STATUS "Release preflight passed: ${REF_NAME} matches the CLI and IDE versions, and its release notes exist.")
