# Static checks of .github/workflows/release.yml and of the repository's own
# release platform policy. A real publication cannot be run in a test, so
# this checks that the workflow is wired to the policy and fails closed;
# scripts/release-preflight.cmake and release-checksums.cmake have their own
# fixture tests.
#   cmake -DSOURCE_DIR=<repo> -DPROJECT_VERSION=<X.Y.Z> -DWORK_DIR=<scratch dir>
#         -P release_workflow_test.cmake

cmake_minimum_required(VERSION 3.20)

if (NOT SOURCE_DIR OR NOT PROJECT_VERSION OR NOT WORK_DIR)
    message(FATAL_ERROR "release_workflow_test.cmake needs SOURCE_DIR, PROJECT_VERSION and WORK_DIR.")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(_failures "")
file(READ "${SOURCE_DIR}/.github/workflows/release.yml" _workflow)
string(REPLACE "\r\n" "\n" _workflow "${_workflow}")

# The text of one job: from "  <job>:" to the next top-level job.
function(job_text name out_var)
    string(FIND "${_workflow}" "\n  ${name}:\n" _start)
    if (_start EQUAL -1)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    math(EXPR _start "${_start} + 1")
    string(SUBSTRING "${_workflow}" ${_start} -1 _rest)
    string(REGEX REPLACE "\n  [A-Za-z0-9_-]+:\n.*$" "\n" _job "${_rest}")
    set(${out_var} "${_job}" PARENT_SCOPE)
endfunction()

function(require where text fragment why)
    string(FIND "${text}" "${fragment}" _at)
    if (_at EQUAL -1)
        set(_failures "${_failures};${where}: lacks '${fragment}' (${why})" PARENT_SCOPE)
    endif()
endfunction()
function(forbid where text fragment why)
    string(FIND "${text}" "${fragment}" _at)
    if (NOT _at EQUAL -1)
        set(_failures "${_failures};${where}: has '${fragment}' (${why})" PARENT_SCOPE)
    endif()
endfunction()
function(count_of text fragment out_var)
    string(LENGTH "${fragment}" _len)
    set(_count 0)
    set(_rest "${text}")
    while (TRUE)
        string(FIND "${_rest}" "${fragment}" _at)
        if (_at EQUAL -1)
            break()
        endif()
        math(EXPR _count "${_count} + 1")
        math(EXPR _next "${_at} + ${_len}")
        string(SUBSTRING "${_rest}" ${_next} -1 _rest)
    endwhile()
    set(${out_var} ${_count} PARENT_SCOPE)
endfunction()

job_text(preflight _preflight)
job_text(build _build)
job_text(ide-windows _ide)
job_text(draft _draft)
job_text(publish _publish)
foreach(_job IN ITEMS preflight build ide-windows draft publish)
    string(REPLACE "-" "_" _var "${_job}")
    if (_job STREQUAL "ide-windows")
        set(_var ide)
    endif()
    if ("${_${_var}}" STREQUAL "")
        list(APPEND _failures "release.yml has no '${_job}' job")
    endif()
endforeach()

# The policy decides what is built and published.
require(preflight "${_preflight}" "\"-DOUTPUT_FILE=$GITHUB_OUTPUT\"" "the policy is passed on as job outputs")
require(build "${_build}" "matrix: \${{ fromJSON(needs.preflight.outputs.build_matrix) }}" "only the policy's CLI packages are built")
require(build "${_build}" "needs: preflight" "nothing is built before the preflight passes")
require(ide-windows "${_ide}" "needs.preflight.outputs.windows_ide == 'true'" "the Windows IDE is built only when the policy names it")
require(ide-windows "${_ide}" "needs: preflight" "nothing is built before the preflight passes")

# Step 1: a draft with exactly the policy's assets, created only after the
# preflight and every package the policy names succeeded.
require(draft "${_draft}" "needs: [preflight, build, ide-windows]" "the draft waits for every packaging job")
require(draft "${_draft}" "needs.preflight.result == 'success'" "a failed preflight stops publication")
require(draft "${_draft}" "needs.build.result == 'success'" "a failed CLI package stops publication")
require(draft "${_draft}" "needs.ide-windows.result == 'skipped' && needs.preflight.outputs.windows_ide != 'true'"
    "the IDE job may be missing only when the policy leaves it out")
require(draft "${_draft}" "EXPECTED: \${{ needs.preflight.outputs.expected }}" "the expected assets are the policy's")
require(draft "${_draft}" "\"-DEXPECTED=$EXPECTED\"" "SHA256SUMS lists exactly the policy's assets")
forbid(draft "${_draft}" "-DEXPECTED=arn-" "no fixed asset list")
require(draft "${_draft}" "Refuse to touch an existing release" "a rerun never adds to or replaces a release")
require(draft "${_draft}" "Check that no left-out Windows package is present" "left-out Windows packages fail closed")
require(draft "${_draft}" "sha256sum --check --strict SHA256SUMS" "an independent check of the manifest")
require(draft "${_draft}" "draft: true" "nothing is published by the tag push")
require(draft "${_draft}" "prerelease: \${{ needs.preflight.outputs.prerelease }}" "the prerelease flag is the policy's")
require(draft "${_draft}" "make_latest: \${{ needs.preflight.outputs.make_latest }}" "the latest flag is the policy's")
require(draft "${_draft}" "overwrite_files: false" "existing assets are never replaced")
require(draft "${_draft}" "fail_on_unmatched_files: true" "a missing file fails")
require(draft "${_draft}" "Check the draft" "the draft is checked after it is created")
forbid(draft "${_draft}" "gh release edit" "the draft job never publishes")

# Step 2: publishing is a separate, manual run on the tag.
require(publish "${_publish}" "github.event_name == 'workflow_dispatch' && inputs.publish_draft" "publishing needs a manual run")
require(publish "${_publish}" "needs: preflight" "the tag's policy is checked again")
require(publish "${_publish}" "sha256sum --check --strict SHA256SUMS" "the draft's files are checked before publishing")
require(publish "${_publish}" "gh release edit \"$TAG\" --draft=false \"--prerelease=$PRERELEASE\" \"--latest=$MAKE_LATEST\""
    "published as the policy says")
require(publish "${_publish}" "became the latest release, but its policy says it must not" "the result is checked")

# Exactly one place creates a release (the draft) and one publishes it; no
# job publishes directly.
count_of("${_workflow}" "softprops/action-gh-release@" _creators)
count_of("${_workflow}" "gh release edit" _publishers)
if (NOT _creators EQUAL 1 OR NOT _publishers EQUAL 1)
    list(APPEND _failures "release.yml: ${_creators} release creation steps and ${_publishers} publication steps, expected 1 and 1")
endif()
forbid(release.yml "${_workflow}" "draft: false" "a release is never created already published")
forbid(release.yml "${_workflow}" "overwrite_files: true" "existing assets are never replaced")
forbid(release.yml "${_workflow}" "make_latest: true" "the latest flag comes from the policy")

# The repository's own policy, when it is the policy of the current version:
# the preflight accepts it, and an incomplete policy is a prerelease that is
# not made latest and publishes no Windows package it leaves out.
file(READ "${SOURCE_DIR}/release/platforms.json" _policy)
string(JSON _policy_version ERROR_VARIABLE _e GET "${_policy}" version)
if (_e)
    list(APPEND _failures "release/platforms.json has no version")
elseif (NOT _policy_version STREQUAL PROJECT_VERSION)
    message(STATUS "release/platforms.json is for ${_policy_version}, not ${PROJECT_VERSION}; its preflight check is skipped.")
else()
    set(_out "${WORK_DIR}/policy.out")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DREPO_ROOT=${SOURCE_DIR}" -DREF_TYPE=tag "-DREF_NAME=v${PROJECT_VERSION}"
                "-DOUTPUT_FILE=${_out}" -P "${SOURCE_DIR}/scripts/release-preflight.cmake"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if (NOT _result EQUAL 0)
        string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_stdout} ${_stderr}")
        list(APPEND _failures "the preflight rejects the repository's policy for v${PROJECT_VERSION}: ${_text}")
    else()
        file(STRINGS "${_out}" _lines)
        foreach(_line IN LISTS _lines)
            string(FIND "${_line}" "=" _eq)
            string(SUBSTRING "${_line}" 0 ${_eq} _key)
            math(EXPR _value_at "${_eq} + 1")
            string(SUBSTRING "${_line}" ${_value_at} -1 _value_${_key})
        endforeach()
        string(REPLACE "," ";" _listed "${_value_platforms}")
        list(LENGTH _listed _listed_count)
        if (_listed_count LESS 5)
            if (NOT _value_prerelease STREQUAL "true" OR NOT _value_make_latest STREQUAL "false")
                list(APPEND _failures "the repository's incomplete policy is not a prerelease kept from latest")
            endif()
        endif()
        if (NOT "windows-x64" IN_LIST _listed AND _value_expected MATCHES "arn-windows-x64\\.zip")
            list(APPEND _failures "the policy leaves out windows-x64 but expects its archive")
        endif()
        if (NOT "windows-ide-x64" IN_LIST _listed AND (_value_windows_ide STREQUAL "true" OR _value_expected MATCHES "arn-ide-"))
            list(APPEND _failures "the policy leaves out windows-ide-x64 but builds or expects it")
        endif()
        message(STATUS "Repository policy for v${PROJECT_VERSION}: platforms ${_value_platforms}; "
                       "prerelease ${_value_prerelease}; make_latest ${_value_make_latest}; assets ${_value_expected}.")
    endif()
endif()

list(FILTER _failures EXCLUDE REGEX "^$")
if (_failures)
    list(JOIN _failures "\n  - " _report)
    message(FATAL_ERROR "Release workflow test failed:\n  - ${_report}")
endif()
message(STATUS "Release workflow tests passed.")
