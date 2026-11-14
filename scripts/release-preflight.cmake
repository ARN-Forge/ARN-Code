# Release preflight: checks that a release run is allowed before anything is
# built. Run by .github/workflows/release.yml and by tests/release_preflight_test.cmake.
#
#   cmake -DREF_TYPE=<tag|branch> -DREF_NAME=<name> [-DREPO_ROOT=<dir>]
#         [-DOUTPUT_FILE=<file>] -P scripts/release-preflight.cmake
#
# Requirements:
#   - the run is for a Git tag (a manual run on a branch such as main must not
#     publish a release named after the branch);
#   - the tag is vX.Y.Z;
#   - X.Y.Z equals project(VERSION) in CMakeLists.txt and the IDE versions in
#     ide/package.json, ide/package-lock.json, ide/src-tauri/tauri.conf.json,
#     ide/src-tauri/Cargo.toml and ide/src-tauri/Cargo.lock;
#   - docs/releases/vX.Y.Z.md exists and is not empty;
#   - release/platforms.json, the release platform policy, is valid for X.Y.Z
#     (see below).
# Every problem is reported; the script then fails once.
#
# The release platform policy (release/platforms.json) states which packages
# this release publishes; nothing is inferred from which jobs happen to run:
#   { "format": "arn-release-platforms/1",
#     "version": "X.Y.Z",                    the tag's version, so a policy is
#                                            never carried over by accident
#     "platforms": ["linux-x64", ...],       at least one CLI platform, each
#                                            once, from the table below
#     "prerelease": true|false,              must be true unless every
#                                            platform is listed
#     "reason": "..." }                      why platforms are missing;
#                                            required when any is
# With OUTPUT_FILE (the workflow passes $GITHUB_OUTPUT), the policy is written
# there as key=value lines: build_matrix (the CLI packages to build, as a
# matrix), windows_ide, expected (the release assets, ;-separated),
# prerelease and make_latest (true/false), and platforms.

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

# 6. Release platform policy. Every known platform: its packaging job and the
# release assets it produces. CLI platforms are built by the "build" matrix
# (name, runner image, artifact name, archive); windows-ide-x64 by the
# "ide-windows" job. Linux uses a fixed image, not ubuntu-latest: the build's
# glibc and libstdc++ set the oldest distribution the binary runs on.
set(_known_platforms windows-x64 linux-x64 macos-arm64 macos-x64 windows-ide-x64)
set(_cli_windows-x64 "Windows x64|windows-latest|arn-windows-x64|arn-windows-x64.zip")
set(_cli_linux-x64 "Linux x64|ubuntu-24.04|arn-linux-x64|arn-linux-x64.tar.gz")
set(_cli_macos-arm64 "macOS arm64|macos-latest|arn-macos-arm64|arn-macos-arm64.tar.gz")
set(_cli_macos-x64 "macOS x64|macos-15-intel|arn-macos-x64|arn-macos-x64.tar.gz")
set(_ide_assets "arn-ide-windows-x64.zip;arn-ide-windows-x64-setup.exe")

set(_policy_file "release/platforms.json")
set(_platforms "")
set(_prerelease "")
_preflight_read("${_policy_file}" _policy)
if (NOT _policy STREQUAL "")
    string(JSON _policy_type ERROR_VARIABLE _policy_error TYPE "${_policy}")
    if (_policy_error OR NOT _policy_type STREQUAL "OBJECT")
        _preflight_error("${_policy_file} is not a JSON object.")
    else()
        string(JSON _format ERROR_VARIABLE _e GET "${_policy}" format)
        if (_e OR NOT _format STREQUAL "arn-release-platforms/1")
            _preflight_error("${_policy_file} must have \"format\": \"arn-release-platforms/1\".")
        endif()
        string(JSON _policy_version ERROR_VARIABLE _e GET "${_policy}" version)
        if (_e)
            _preflight_error("${_policy_file} has no \"version\".")
        elseif (NOT _policy_version STREQUAL _version)
            _preflight_error("${_policy_file} is the policy for ${_policy_version}, but the tag is ${REF_NAME}; write the policy for this release.")
        endif()
        string(JSON _list_type ERROR_VARIABLE _e TYPE "${_policy}" platforms)
        if (_e OR NOT _list_type STREQUAL "ARRAY")
            _preflight_error("${_policy_file} has no \"platforms\" list.")
        else()
            string(JSON _count LENGTH "${_policy}" platforms)
            if (_count EQUAL 0)
                _preflight_error("${_policy_file} lists no platforms.")
            else()
                math(EXPR _last "${_count} - 1")
                foreach(_i RANGE ${_last})
                    string(JSON _item_type TYPE "${_policy}" platforms ${_i})
                    string(JSON _item GET "${_policy}" platforms ${_i})
                    if (NOT _item_type STREQUAL "STRING" OR NOT _item IN_LIST _known_platforms)
                        _preflight_error("${_policy_file} lists '${_item}', which is not one of: ${_known_platforms}.")
                    elseif (_item IN_LIST _platforms)
                        _preflight_error("${_policy_file} lists '${_item}' twice.")
                    else()
                        list(APPEND _platforms "${_item}")
                    endif()
                endforeach()
            endif()
        endif()
        string(JSON _pre_type ERROR_VARIABLE _e TYPE "${_policy}" prerelease)
        if (_e OR NOT _pre_type STREQUAL "BOOLEAN")
            _preflight_error("${_policy_file} must have \"prerelease\": true or false.")
        else()
            string(JSON _prerelease GET "${_policy}" prerelease)
        endif()
        set(_reason "")
        string(JSON _reason_type ERROR_VARIABLE _reason_error TYPE "${_policy}" reason)
        if (NOT _reason_error)
            if (_reason_type STREQUAL "STRING")
                string(JSON _reason GET "${_policy}" reason)
                string(STRIP "${_reason}" _reason)
            else()
                _preflight_error("${_policy_file} has a \"reason\" that is not a string.")
            endif()
        endif()
        # CMake's JSON parser keeps only one of two equal keys without saying
        # so, so a key written twice is found in the text: in this flat object
        # no string value can contain an unescaped "<key>": sequence.
        foreach(_key IN ITEMS format version platforms prerelease reason)
            string(REGEX MATCHALL "\"${_key}\"[ \t\r\n]*:" _hits "${_policy}")
            list(LENGTH _hits _hit_count)
            if (_hit_count GREATER 1)
                _preflight_error("${_policy_file} has the key \"${_key}\" ${_hit_count} times.")
            endif()
        endforeach()
    endif()
endif()

set(_missing "")
foreach(_platform IN LISTS _known_platforms)
    if (NOT _platform IN_LIST _platforms)
        list(APPEND _missing "${_platform}")
    endif()
endforeach()
list(JOIN _missing ", " _missing_text)
set(_cli_count 0)
foreach(_platform IN LISTS _platforms)
    if (DEFINED _cli_${_platform})
        math(EXPR _cli_count "${_cli_count} + 1")
    endif()
endforeach()
if (_platforms AND _cli_count EQUAL 0)
    _preflight_error("${_policy_file} lists no CLI platform; a release must publish at least one CLI package.")
endif()
if (_missing AND _prerelease STREQUAL "OFF")
    _preflight_error("${_policy_file} leaves out ${_missing_text}, so the release is incomplete and must have \"prerelease\": true; an incomplete release is never published as the latest stable release.")
endif()
if (_missing AND _platforms AND _reason STREQUAL "")
    _preflight_error("${_policy_file} leaves out ${_missing_text} but gives no \"reason\".")
endif()

list(FILTER _errors EXCLUDE REGEX "^$")
if (_errors)
    set(_report "Release preflight failed for ${REF_NAME}:")
    foreach(_error IN LISTS _errors)
        string(APPEND _report "\n  - ${_error}")
    endforeach()
    message(FATAL_ERROR "${_report}")
endif()

# The policy, for the workflow: the build matrix and the exact release assets
# (in the fixed order of the table above, whatever the policy's order), and
# how the release is marked.
set(_matrix "")
set(_expected "")
foreach(_platform IN LISTS _known_platforms)
    if (NOT _platform IN_LIST _platforms)
        continue()
    endif()
    if (DEFINED _cli_${_platform})
        string(REPLACE "|" ";" _fields "${_cli_${_platform}}")
        list(GET _fields 0 _name)
        list(GET _fields 1 _os)
        list(GET _fields 2 _artifact)
        list(GET _fields 3 _archive)
        if (NOT _matrix STREQUAL "")
            string(APPEND _matrix ",")
        endif()
        string(APPEND _matrix "{\"name\":\"${_name}\",\"os\":\"${_os}\",\"artifact\":\"${_artifact}\",\"archive\":\"${_archive}\"}")
        list(APPEND _expected "${_archive}")
    else()
        list(APPEND _expected ${_ide_assets})
    endif()
endforeach()
if ("windows-ide-x64" IN_LIST _platforms)
    set(_windows_ide true)
else()
    set(_windows_ide false)
endif()
if (_prerelease STREQUAL "ON")
    set(_pre true)
    set(_latest false)
else()
    set(_pre false)
    set(_latest true)
endif()
list(JOIN _platforms "," _platform_text)
if (OUTPUT_FILE)
    file(APPEND "${OUTPUT_FILE}"
        "build_matrix={\"include\":[${_matrix}]}\n"
        "windows_ide=${_windows_ide}\n"
        "expected=${_expected}\n"
        "prerelease=${_pre}\n"
        "make_latest=${_latest}\n"
        "platforms=${_platform_text}\n")
endif()
if (_missing)
    set(_kind "a prerelease of ${_platform_text} only (left out: ${_missing_text}. ${_reason})")
elseif (_pre)
    set(_kind "a complete prerelease")
else()
    set(_kind "a complete release, published as the latest release")
endif()
message(STATUS "Release preflight passed: ${REF_NAME} matches the CLI and IDE versions, its release notes exist, "
               "and it is ${_kind}. Assets: ${_expected}.")
