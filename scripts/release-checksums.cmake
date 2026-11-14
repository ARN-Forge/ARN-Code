# Collects the release assets and writes their SHA256SUMS manifest.
#
#   cmake -DARTIFACTS_DIR=<dir> -DDIST_DIR=<dir> "-DEXPECTED=<name>;<name>;..."
#         -P scripts/release-checksums.cmake
#
# ARTIFACTS_DIR holds one subdirectory per downloaded workflow artifact.
# Every file found there is a release asset. The script fails, writing
# nothing, unless the assets are exactly the EXPECTED file names: none
# missing, none extra, no name twice (two artifacts with the same file name
# would otherwise silently overwrite each other), every name a plain file name,
# and no file empty. It then copies the assets into DIST_DIR (which must not
# exist yet) and writes DIST_DIR/SHA256SUMS in the format of sha256sum:
#   <64 lower-case hex digits><two spaces><file name>
# one line per asset, sorted by file name, LF line endings, without an entry
# for SHA256SUMS itself.

cmake_minimum_required(VERSION 3.20)

if (NOT ARTIFACTS_DIR OR NOT DIST_DIR OR NOT EXPECTED)
    message(FATAL_ERROR "release-checksums.cmake needs ARTIFACTS_DIR, DIST_DIR and EXPECTED.")
endif()
if (NOT IS_DIRECTORY "${ARTIFACTS_DIR}")
    message(FATAL_ERROR "Release checksums: ${ARTIFACTS_DIR} is not a directory.")
endif()
if (EXISTS "${DIST_DIR}")
    message(FATAL_ERROR "Release checksums: ${DIST_DIR} already exists, but it must start empty.")
endif()

set(_errors "")
foreach(_name IN LISTS EXPECTED)
    if (NOT _name MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$" OR _name STREQUAL "SHA256SUMS")
        list(APPEND _errors "expected asset name '${_name}' is not a plain file name")
    endif()
endforeach()
set(_expected ${EXPECTED})
list(REMOVE_DUPLICATES _expected)
list(LENGTH EXPECTED _expected_count)
list(LENGTH _expected _unique_count)
if (NOT _expected_count EQUAL _unique_count)
    list(APPEND _errors "the expected asset list names a file twice")
endif()

file(GLOB_RECURSE _files LIST_DIRECTORIES false "${ARTIFACTS_DIR}/*")
set(_names "")
foreach(_file IN LISTS _files)
    get_filename_component(_name "${_file}" NAME)
    if (_name IN_LIST _names)
        list(APPEND _errors "two artifacts contain '${_name}'")
        continue()
    endif()
    list(APPEND _names "${_name}")
    set(_path_of_${_name} "${_file}")
    if (NOT _name IN_LIST _expected)
        list(APPEND _errors "unexpected asset '${_name}'")
        continue()
    endif()
    file(SIZE "${_file}" _size)
    if (_size EQUAL 0)
        list(APPEND _errors "asset '${_name}' is empty")
    endif()
endforeach()
foreach(_name IN LISTS _expected)
    if (NOT _name IN_LIST _names)
        list(APPEND _errors "expected asset '${_name}' is missing")
    endif()
endforeach()

if (_errors)
    list(JOIN _errors "\n  - " _report)
    message(FATAL_ERROR "Release checksums failed:\n  - ${_report}")
endif()

list(SORT _expected COMPARE STRING CASE SENSITIVE)
file(MAKE_DIRECTORY "${DIST_DIR}")
set(_manifest "")
foreach(_name IN LISTS _expected)
    configure_file("${_path_of_${_name}}" "${DIST_DIR}/${_name}" COPYONLY)
    file(SHA256 "${DIST_DIR}/${_name}" _hash)
    string(APPEND _manifest "${_hash}  ${_name}\n")
endforeach()
# CONFIGURE writes the newlines as LF on every platform (file(WRITE) would use
# CRLF on Windows); @ONLY with no @-variables leaves the text unchanged.
file(CONFIGURE OUTPUT "${DIST_DIR}/SHA256SUMS" CONTENT "${_manifest}" @ONLY NEWLINE_STYLE UNIX)
string(STRIP "${_manifest}" _summary)
message(STATUS "Release checksums written to ${DIST_DIR}/SHA256SUMS:\n${_summary}")
