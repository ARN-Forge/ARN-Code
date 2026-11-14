cmake_minimum_required(VERSION 3.20)

project(ArnAgentCode VERSION 0.9.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_options(/utf-8 /EHsc)
endif()

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

# cpp-httplib comes only from ARN Core, which ships a patched copy (per-request
# cancellation). Declaring another copy here would stop ARN Core from
# configuring, because two copies would disagree on httplib::Request.
FetchContent_MakeAvailable(nlohmann_json)

find_package(Threads REQUIRED)

include(CTest)
option(ARN_ENABLE_KIRO_INTEGRATION_TESTS "Run tests using an installed/authenticated Kiro CLI" OFF)
option(ARN_ENABLE_PTY_TESTS "Run the POSIX terminal tests on a real pseudo-terminal (needs bash, tmux and node)" OFF)

# ARN Core dependency
# Supports:
# 1. Local development: checks ARN_CORE_SOURCE_DIR or adjacent ../arn-core repository
# 2. Installed package: find_package(arn_core 0.1.0 CONFIG QUIET)
# 3. Remote/CI fallback: FetchContent from a tested arn-core revision

set(ARN_CORE_SOURCE_DIR "" CACHE PATH "Path to standalone arn-core repository")

if (NOT ARN_CORE_SOURCE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../arn-core/CMakeLists.txt")
    set(ARN_CORE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../arn-core")
endif()

if (ARN_CORE_SOURCE_DIR)
    message(STATUS "Consuming ARN Core from local source: ${ARN_CORE_SOURCE_DIR}")
    add_subdirectory("${ARN_CORE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/arn_core_subproject")
else()
    find_package(arn_core 0.1.0 CONFIG QUIET)
    if (arn_core_FOUND)
        message(STATUS "Found installed arn_core: ${arn_core_DIR}")
    else()
        set(ARN_CORE_GIT_REVISION "3db01b6d71e02682c6d332278ce4aa78ad81f47b")
        message(STATUS "Fetching arn_core ${ARN_CORE_GIT_REVISION} from https://github.com/ARN-Forge/arn-core.git")
        FetchContent_Declare(
            arn_core
            GIT_REPOSITORY https://github.com/ARN-Forge/arn-core.git
            GIT_TAG ${ARN_CORE_GIT_REVISION}
        )
        FetchContent_MakeAvailable(arn_core)
    endif()
endif()


if (WIN32)
    set(ARN_TERMINAL_SOURCE src/terminal_windows.cpp)
else()
    set(ARN_TERMINAL_SOURCE src/terminal_posix.cpp)
endif()

# Build identity: `arn --version` prints the version and the source commit.
# cmake/ArnBuildInfo.cmake regenerates arn_build_info.hpp on every build (only
# rewriting it when it changes), so a new commit or local edit is never shown
# with a stale identity. Without Git it reports "unknown"; packagers can pass
# the commit explicitly with -DARN_BUILD_COMMIT=<sha>.
find_package(Git QUIET)
set(ARN_BUILD_COMMIT "" CACHE STRING
    "Source commit for arn --version when the source tree has no Git metadata (12-40 hex digits)")
set(ARN_BUILD_INFO_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_arn_build_info_command
    "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DOUTPUT=${ARN_BUILD_INFO_DIR}/arn_build_info.hpp"
    "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    "-DEXPLICIT_COMMIT=${ARN_BUILD_COMMIT}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ArnBuildInfo.cmake")
execute_process(COMMAND ${_arn_build_info_command} RESULT_VARIABLE _arn_build_info_result)
if (NOT _arn_build_info_result EQUAL 0)
    message(FATAL_ERROR "Could not determine the ARN build identity.")
endif()
add_custom_target(arn_build_info
    COMMAND ${_arn_build_info_command}
    BYPRODUCTS "${ARN_BUILD_INFO_DIR}/arn_build_info.hpp"
    COMMENT "Checking the ARN build identity"
    VERBATIM)

add_executable(arn
    src/main.cpp
    src/build_info.cpp
    src/model_provider.cpp
    src/normal_chat.cpp
    src/api_client.cpp
    src/agent_workflow.cpp
    src/cli_support.cpp
    src/config_manager.cpp
    src/acp/acp_subprocess.cpp
    src/acp/acp_client.cpp
    src/acp/kiro_adapter.cpp
    src/acp/acp_session.cpp
    src/tools/workspace_sandbox.cpp
    src/tools/coding_tools.cpp
    src/server.cpp
    ${ARN_TERMINAL_SOURCE}
)

target_include_directories(arn PRIVATE include "${ARN_BUILD_INFO_DIR}")
target_compile_definitions(arn PRIVATE ARN_VERSION="${PROJECT_VERSION}")
add_dependencies(arn arn_build_info)
target_link_libraries(arn PRIVATE arn::core nlohmann_json::nlohmann_json Threads::Threads)
if (MSVC)
    target_compile_options(arn PRIVATE /utf-8)
    # The Microsoft Visual C++ runtime is not packaged with arn.exe: it is a
    # prerequisite that users install from Microsoft (the Visual C++
    # Redistributable), and scripts/install.ps1 checks for it.
endif()

if (BUILD_TESTING)
    add_executable(model_list_test tests/model_list_test.cpp)
    target_include_directories(model_list_test PRIVATE include)
    target_link_libraries(model_list_test PRIVATE arn::core nlohmann_json::nlohmann_json)
    add_test(NAME model_list COMMAND model_list_test)
    add_executable(model_provider_test tests/model_provider_test.cpp src/model_provider.cpp)
    target_include_directories(model_provider_test PRIVATE include)
    target_link_libraries(model_provider_test PRIVATE arn::core nlohmann_json::nlohmann_json Threads::Threads)
    add_test(NAME model_provider COMMAND model_provider_test)
    if (MSVC)
        target_compile_options(model_provider_test PRIVATE /utf-8)
    endif()
    add_executable(acp_client_test
        tests/acp_client_test.cpp
        src/acp/acp_subprocess.cpp
        src/acp/acp_client.cpp
        src/acp/kiro_adapter.cpp
        src/acp/acp_session.cpp)
    find_program(ARN_TEST_NODE_EXECUTABLE NAMES node nodejs REQUIRED)
    target_include_directories(acp_client_test PRIVATE include)
    target_compile_definitions(acp_client_test PRIVATE
        ARN_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
        ARN_TEST_NODE="${ARN_TEST_NODE_EXECUTABLE}")
    target_link_libraries(acp_client_test PRIVATE
        arn::core nlohmann_json::nlohmann_json Threads::Threads)
    if (MSVC)
        target_compile_options(acp_client_test PRIVATE /utf-8)
    endif()
    add_test(NAME acp_client COMMAND acp_client_test)
    set_tests_properties(acp_client PROPERTIES TIMEOUT 60)
    add_executable(acp_session_test tests/acp_session_test.cpp
        src/acp/acp_subprocess.cpp src/acp/acp_client.cpp
        src/acp/kiro_adapter.cpp src/acp/acp_session.cpp)
    target_include_directories(acp_session_test PRIVATE include)
    target_compile_definitions(acp_session_test PRIVATE
        ARN_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
        ARN_TEST_NODE="${ARN_TEST_NODE_EXECUTABLE}")
    target_link_libraries(acp_session_test PRIVATE
        arn::core nlohmann_json::nlohmann_json Threads::Threads)
    add_test(NAME acp_session COMMAND acp_session_test)
    set_tests_properties(acp_session PROPERTIES TIMEOUT 60)
    # IDE security configuration (no CDN code, CSP, capabilities, IPC
    # commands); the built bundle and the running IDE are checked by the IDE
    # jobs of the workflows.
    add_test(NAME ide_security_config
        COMMAND ${ARN_TEST_NODE_EXECUTABLE} --test ${CMAKE_CURRENT_SOURCE_DIR}/tests/ide_security_test.mjs)
    set_tests_properties(ide_security_config PROPERTIES TIMEOUT 60)
    add_executable(acp_subprocess_test tests/acp_subprocess_test.cpp src/acp/acp_subprocess.cpp)
    target_include_directories(acp_subprocess_test PRIVATE include)
    target_link_libraries(acp_subprocess_test PRIVATE Threads::Threads)
    add_test(NAME acp_subprocess COMMAND acp_subprocess_test)
    set_tests_properties(acp_subprocess PROPERTIES TIMEOUT 30)

    add_executable(acp_smoke_test
        tests/acp_smoke_test.cpp
        src/acp/acp_subprocess.cpp
        src/acp/acp_client.cpp
        src/acp/kiro_adapter.cpp
        src/acp/acp_session.cpp)
    target_include_directories(acp_smoke_test PRIVATE include)
    target_link_libraries(acp_smoke_test PRIVATE
        arn::core nlohmann_json::nlohmann_json Threads::Threads)
    if (MSVC)
        target_compile_options(acp_smoke_test PRIVATE /utf-8)
    endif()
    if (ARN_ENABLE_KIRO_INTEGRATION_TESTS)
        add_test(NAME acp_smoke COMMAND acp_smoke_test)
        set_tests_properties(acp_smoke PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77 LABELS integration)
    endif()
endif()

if (BUILD_TESTING)
    add_executable(confirmation_test tests/confirmation_test.cpp
                                     src/tools/workspace_sandbox.cpp
                                     src/tools/coding_tools.cpp)
    target_include_directories(confirmation_test PRIVATE include)
    target_link_libraries(confirmation_test PRIVATE arn::core nlohmann_json::nlohmann_json Threads::Threads)
    add_test(NAME confirmations_and_tools COMMAND confirmation_test)
    set(_arn_code_test_temp "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
    file(MAKE_DIRECTORY "${_arn_code_test_temp}")
    set_tests_properties(confirmations_and_tools PROPERTIES
        ENVIRONMENT "TEMP=${_arn_code_test_temp};TMP=${_arn_code_test_temp}")
    # Links out of the project. Creating a link needs a privilege some systems
    # do not grant; the test then exits with 77 and CTest reports it skipped.
    add_executable(sandbox_symlink_test tests/sandbox_symlink_test.cpp
                                        src/tools/workspace_sandbox.cpp
                                        src/tools/coding_tools.cpp)
    target_include_directories(sandbox_symlink_test PRIVATE include)
    target_link_libraries(sandbox_symlink_test PRIVATE arn::core nlohmann_json::nlohmann_json Threads::Threads)
    add_test(NAME sandbox_symlinks COMMAND sandbox_symlink_test)
    set_tests_properties(sandbox_symlinks PROPERTIES
        SKIP_RETURN_CODE 77
        ENVIRONMENT "TEMP=${_arn_code_test_temp};TMP=${_arn_code_test_temp}")
    add_executable(arn_server_fixture tests/server_fixture.cpp
                                      src/server.cpp
                                      src/tools/workspace_sandbox.cpp
                                      src/tools/coding_tools.cpp)
    target_include_directories(arn_server_fixture PRIVATE include)
    target_link_libraries(arn_server_fixture PRIVATE arn::core nlohmann_json::nlohmann_json Threads::Threads)
    find_package(Python3 COMPONENTS Interpreter)
    if (Python3_FOUND)
        add_test(NAME server_protocol COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/server_protocol_test.py $<TARGET_FILE:arn_server_fixture>)
        set_tests_properties(server_protocol PROPERTIES
            TIMEOUT 30
            ENVIRONMENT "TEMP=${_arn_code_test_temp};TMP=${_arn_code_test_temp}")
    else()
        message(STATUS "Python 3 not found: the server_protocol test is not registered")
    endif()
    if (WIN32)
        find_program(ARN_POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
        add_test(NAME windows_install
            COMMAND ${ARN_POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_windows_test.ps1
                    -BuildDir $<TARGET_FILE_DIR:arn>)
        # Both skip (77) for a Debug build, whose debug runtime cannot ship.
        set_tests_properties(windows_install PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)
        # Release ZIP verification (scripts/verify-windows-package.ps1).
        add_test(NAME windows_package_verification
            COMMAND ${ARN_POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_windows_test.ps1
                    -BuildDir $<TARGET_FILE_DIR:arn>)
        set_tests_properties(windows_package_verification PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
        # SHA256SUMS verification and version selection in scripts/install.ps1.
        add_test(NAME windows_install_checksums
            COMMAND ${ARN_POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_checksum_test.ps1
                    -BuildDir $<TARGET_FILE_DIR:arn>)
        set_tests_properties(windows_install_checksums PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
        # Release resolution and downgrade protection in scripts/install.ps1,
        # against a mocked GitHub API.
        add_test(NAME windows_install_resolver
            COMMAND ${ARN_POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_resolver_test.ps1
                    -BuildDir $<TARGET_FILE_DIR:arn>)
        set_tests_properties(windows_install_resolver PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
        # Upgrade, rollback, installation manifest and uninstall in scripts/install.ps1.
        add_test(NAME windows_install_upgrade
            COMMAND ${ARN_POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_upgrade_test.ps1
                    -BuildDir $<TARGET_FILE_DIR:arn>)
        set_tests_properties(windows_install_upgrade PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
        # Crash recovery, PATH handling and uninstall preflight in scripts/install.ps1.
        add_test(NAME windows_install_recovery
            COMMAND ${ARN_POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_recovery_test.ps1
                    -BuildDir $<TARGET_FILE_DIR:arn>)
        set_tests_properties(windows_install_recovery PROPERTIES TIMEOUT 400 SKIP_RETURN_CODE 77)
        # IDE package verification (scripts/verify-ide-package.ps1) against
        # stand-ins for arn.exe and arn-ide.exe: the current version and
        # commit, a "-dirty" build, and an IDE with an older version resource.
        function(arn_ide_package_fixture target rc_version version_line)
            set(ARN_FIXTURE_RC_VERSION "${rc_version}")
            configure_file(tests/ide_package_fixture.rc.in
                           "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc" @ONLY)
            add_executable(${target} tests/ide_package_fixture.cpp
                           "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc")
            target_compile_definitions(${target} PRIVATE "ARN_FIXTURE_VERSION_LINE=\"${version_line}\"")
        endfunction()
        arn_ide_package_fixture(ide_fixture_current "${PROJECT_VERSION}" "arn ${PROJECT_VERSION} (0123456789ab)")
        arn_ide_package_fixture(ide_fixture_dirty "${PROJECT_VERSION}" "arn ${PROJECT_VERSION} (0123456789ab-dirty)")
        arn_ide_package_fixture(ide_fixture_old "0.0.1" "arn 0.0.1 (0123456789ab)")
        add_test(NAME ide_package_verification
            COMMAND ${ARN_POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/ide_package_test.ps1
                    -FixtureDir $<TARGET_FILE_DIR:ide_fixture_current>
                    -BuildDir $<TARGET_FILE_DIR:arn>
                    -Version ${PROJECT_VERSION})
        set_tests_properties(ide_package_verification PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    else()
        # Release tar.gz packaging and verification (scripts/package-unix.sh);
        # Python writes the malformed test archives.
        find_program(ARN_BASH_EXECUTABLE NAMES bash)
        if (Python3_FOUND AND ARN_BASH_EXECUTABLE)
            add_test(NAME unix_package_verification
                COMMAND ${ARN_BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_unix_test.sh
                        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package-unix.sh ${Python3_EXECUTABLE})
            set_tests_properties(unix_package_verification PROPERTIES TIMEOUT 120)
        else()
            message(STATUS "Python 3 or bash not found: the unix_package_verification test is not registered")
        endif()
        # SHA256SUMS verification and version selection in scripts/install.sh;
        # skips (77) where install.sh has no release for the platform.
        if (ARN_BASH_EXECUTABLE)
            add_test(NAME unix_install_checksums
                COMMAND ${ARN_BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_unix_test.sh
                        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install.sh)
            set_tests_properties(unix_install_checksums PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
            # Release resolution and downgrade protection in scripts/install.sh,
            # against a mocked GitHub API, for every platform it supports.
            add_test(NAME unix_install_resolver
                COMMAND ${ARN_BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_resolver_test.sh
                        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install.sh)
            set_tests_properties(unix_install_resolver PROPERTIES TIMEOUT 300)
            # Upgrade, rollback, installation manifest and uninstall in scripts/install.sh.
            add_test(NAME unix_install_upgrade
                COMMAND ${ARN_BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_unix_upgrade_test.sh
                        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install.sh)
            set_tests_properties(unix_install_upgrade PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
            # Crash recovery, signal handling and profile safety in scripts/install.sh.
            add_test(NAME unix_install_recovery
                COMMAND ${ARN_BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_unix_recovery_test.sh
                        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install.sh)
            set_tests_properties(unix_install_recovery PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
            # The GitHub side of the release workflow (scripts/release-publish.sh)
            # against a fake gh and curl; skips (77) without jq.
            add_test(NAME release_publish
                COMMAND ${ARN_BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_publish_test.sh
                        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/release-publish.sh)
            set_tests_properties(release_publish PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
            # Release tags only on commits already on main (scripts/release-check-main.sh),
            # with real Git repositories; skips (77) without git.
            add_test(NAME release_main_ancestry
                COMMAND ${ARN_BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_main_ancestry_test.sh
                        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/release-check-main.sh)
            set_tests_properties(release_main_ancestry PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
        endif()
    endif()
    # SHA256SUMS generation for the release (scripts/release-checksums.cmake).
    add_test(NAME release_checksums
        COMMAND ${CMAKE_COMMAND}
                "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/scripts/release-checksums.cmake"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-release-checksums"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_checksums_test.cmake")
    set_tests_properties(release_checksums PROPERTIES TIMEOUT 60)
    # .github/workflows/release.yml follows release/platforms.json and fails
    # closed (static checks; a publication cannot run in a test).
    add_test(NAME release_workflow
        COMMAND ${CMAKE_COMMAND}
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DPROJECT_VERSION=${PROJECT_VERSION}"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-release-workflow"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_workflow_test.cmake")
    set_tests_properties(release_workflow PROPERTIES TIMEOUT 60)
    add_executable(architecture_test tests/architecture_test.cpp)
    if (TARGET arn_core)
        get_target_property(ARN_CORE_SOURCE_DIR_PROP arn_core SOURCE_DIR)
        target_compile_definitions(architecture_test PRIVATE ARN_CORE_DIR="${ARN_CORE_SOURCE_DIR_PROP}")
    elseif (DEFINED ARN_CORE_SOURCE_DIR AND ARN_CORE_SOURCE_DIR)
        target_compile_definitions(architecture_test PRIVATE ARN_CORE_DIR="${ARN_CORE_SOURCE_DIR}")
    elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../arn-core")
        target_compile_definitions(architecture_test PRIVATE ARN_CORE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../arn-core")
    endif()
    target_link_libraries(architecture_test PRIVATE arn::core)
    add_test(NAME architecture_boundary COMMAND architecture_test)

    add_executable(agent_workflow_test tests/agent_workflow_test.cpp
                                       src/agent_workflow.cpp
                                       src/model_provider.cpp
                                       src/tools/workspace_sandbox.cpp
                                       src/tools/coding_tools.cpp)
    target_include_directories(agent_workflow_test PRIVATE include)
    target_link_libraries(agent_workflow_test PRIVATE arn::core nlohmann_json::nlohmann_json Threads::Threads)
    add_test(NAME agent_workflow COMMAND agent_workflow_test)

    add_executable(cli_support_test tests/cli_support_test.cpp src/cli_support.cpp)
    target_include_directories(cli_support_test PRIVATE include)
    add_test(NAME cli_support COMMAND cli_support_test)
    add_executable(confirmation_input_test tests/confirmation_input_test.cpp)
    target_include_directories(confirmation_input_test PRIVATE include)
    add_test(NAME confirmation_input COMMAND confirmation_input_test)
    add_executable(terminal_ui_test tests/terminal_ui_test.cpp)
    target_include_directories(terminal_ui_test PRIVATE include)
    add_test(NAME terminal_ui COMMAND terminal_ui_test)
    # Header-only renderer core (include/tui). Not used by the arn target yet.
    add_executable(tui_renderer_test tests/tui_renderer_test.cpp)
    target_include_directories(tui_renderer_test PRIVATE include)
    if (MSVC)
        target_compile_options(tui_renderer_test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(tui_renderer_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME tui_renderer COMMAND tui_renderer_test)
    if (WIN32)
        add_executable(terminal_windows_test tests/terminal_windows_test.cpp
            src/terminal_windows.cpp src/cli_support.cpp)
        target_include_directories(terminal_windows_test PRIVATE include)
        target_link_libraries(terminal_windows_test PRIVATE Threads::Threads)
        add_test(NAME terminal_windows_input COMMAND terminal_windows_test $<TARGET_FILE:arn>)
        set_tests_properties(terminal_windows_input PROPERTIES TIMEOUT 30)
        # Renderer choice (default alternate screen, unsupported values,
        # redirected I/O) on a real console. Needs a console window that can be
        # resized and read back; without one it exits with 77 and CTest reports
        # it skipped. Run it directly in a console window to exercise it.
        add_executable(fake_kiro_cli tests/fake_kiro_cli.cpp)
        add_executable(terminal_windows_alt_test tests/terminal_windows_alt_test.cpp src/terminal_windows.cpp)
        target_include_directories(terminal_windows_alt_test PRIVATE include)
        target_link_libraries(terminal_windows_alt_test PRIVATE Threads::Threads)
        add_test(NAME terminal_windows_alternate
                 COMMAND terminal_windows_alt_test $<TARGET_FILE:arn> $<TARGET_FILE:fake_kiro_cli>
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/acp_fake_server.mjs)
        set_tests_properties(terminal_windows_alternate PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77)
    else()
        add_executable(terminal_posix_test tests/terminal_posix_test.cpp src/terminal_posix.cpp)
        target_include_directories(terminal_posix_test PRIVATE include)
        target_link_libraries(terminal_posix_test PRIVATE Threads::Threads)
        add_test(NAME terminal_posix COMMAND terminal_posix_test)
        set_tests_properties(terminal_posix PROPERTIES TIMEOUT 60)
        if (ARN_ENABLE_PTY_TESTS)
            add_test(NAME terminal_posix_pty
                     COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/posix_pty_test.sh
                             $<TARGET_FILE:arn> $<TARGET_FILE:terminal_posix_test>
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/acp_fake_server.mjs)
            set_tests_properties(terminal_posix_pty PROPERTIES TIMEOUT 600)
        endif()
    endif()
    add_executable(normal_chat_test tests/normal_chat_test.cpp src/normal_chat.cpp)
    target_include_directories(normal_chat_test PRIVATE include)
    target_link_libraries(normal_chat_test PRIVATE arn::core)
    add_test(NAME normal_chat COMMAND normal_chat_test)

    # Labels select groups (ctest -L unit). Every test has a timeout, so a hang
    # fails within minutes instead of CTest's default 1500 seconds.
    #   unit         runs in-process; starts no other program
    #   integration  starts other programs (ARN, Node, Python, PowerShell, itself)
    #   console      needs a Windows console
    #   pty          needs a pseudo-terminal (tmux)
    # Build identity and release preflight (CMake scripts; Git when available).
    add_test(NAME build_identity
        COMMAND ${CMAKE_COMMAND}
                "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/ArnBuildInfo.cmake"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-build-identity"
                "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_info_test.cmake")
    add_test(NAME version_output
        COMMAND ${CMAKE_COMMAND}
                "-DARN_EXE=$<TARGET_FILE:arn>"
                "-DEXPECTED_VERSION=${PROJECT_VERSION}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/version_output_test.cmake")
    add_test(NAME release_preflight
        COMMAND ${CMAKE_COMMAND}
                "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/scripts/release-preflight.cmake"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-release-preflight"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_preflight_test.cmake")
    set_tests_properties(build_identity version_output release_preflight PROPERTIES TIMEOUT 60)

    set(_arn_unit_tests model_list model_provider confirmations_and_tools sandbox_symlinks
        architecture_boundary agent_workflow cli_support confirmation_input terminal_ui
        tui_renderer normal_chat)
    set_tests_properties(${_arn_unit_tests} PROPERTIES LABELS unit TIMEOUT 60)
    set(_arn_integration_tests acp_client acp_session acp_subprocess
        build_identity version_output release_preflight release_checksums release_workflow)
    if (Python3_FOUND)
        list(APPEND _arn_integration_tests server_protocol)
    endif()
    if (WIN32)
        list(APPEND _arn_integration_tests windows_install windows_package_verification
            windows_install_checksums windows_install_resolver windows_install_upgrade windows_install_recovery)
        set_tests_properties(terminal_windows_input terminal_windows_alternate PROPERTIES
            LABELS "console;integration")
    else()
        foreach(_arn_unix_test IN ITEMS unix_package_verification unix_install_checksums unix_install_resolver
                unix_install_upgrade unix_install_recovery release_publish release_main_ancestry)
            if (TEST ${_arn_unix_test})
                list(APPEND _arn_integration_tests ${_arn_unix_test})
            endif()
        endforeach()
        set_tests_properties(terminal_posix PROPERTIES LABELS unit)
        if (ARN_ENABLE_PTY_TESTS)
            set_tests_properties(terminal_posix_pty PROPERTIES LABELS "pty;integration")
        endif()
    endif()
    set_tests_properties(${_arn_integration_tests} PROPERTIES LABELS integration)

    if (MSVC)
        target_compile_options(confirmation_test PRIVATE /utf-8)
        target_compile_options(arn_server_fixture PRIVATE /utf-8)
        target_compile_options(architecture_test PRIVATE /utf-8)
        target_compile_options(agent_workflow_test PRIVATE /utf-8)
        target_compile_options(cli_support_test PRIVATE /utf-8)
        target_compile_options(normal_chat_test PRIVATE /utf-8)
    endif()
endif()

if (WIN32)
    set(_arn_code_runtime_targets arn)
    if (BUILD_TESTING)
        list(APPEND _arn_code_runtime_targets
            model_list_test model_provider_test confirmation_test sandbox_symlink_test
            arn_server_fixture architecture_test agent_workflow_test)
        list(APPEND _arn_code_runtime_targets cli_support_test normal_chat_test)
        list(APPEND _arn_code_runtime_targets acp_client_test acp_session_test acp_smoke_test)
    endif()
    set(ARN_CODE_RUNTIME_EXECUTABLES "")
    set(ARN_CODE_RUNTIME_DIRS "$<TARGET_FILE_DIR:arn>")
    foreach(_target IN LISTS _arn_code_runtime_targets)
        list(APPEND ARN_CODE_RUNTIME_EXECUTABLES "$<TARGET_FILE:${_target}>")
        list(APPEND ARN_CODE_RUNTIME_DIRS "$<TARGET_FILE_DIR:${_target}>")
    endforeach()
    foreach(_include IN LISTS OPENSSL_INCLUDE_DIR)
        get_filename_component(_prefix "${_include}" DIRECTORY)
        list(APPEND ARN_CODE_RUNTIME_DIRS "${_prefix}/bin" "${_prefix}")
    endforeach()
    if (OPENSSL_ROOT_DIR)
        list(APPEND ARN_CODE_RUNTIME_DIRS "${OPENSSL_ROOT_DIR}/bin" "${OPENSSL_ROOT_DIR}")
    endif()
    foreach(_target OpenSSL::SSL OpenSSL::Crypto)
        if (TARGET ${_target})
            list(APPEND ARN_CODE_RUNTIME_DIRS "$<TARGET_FILE_DIR:${_target}>")
        endif()
    endforeach()
    if (MSVC)
        get_filename_component(_linker_dir "${CMAKE_LINKER}" DIRECTORY)
        find_program(ARN_CODE_PE_SCANNER NAMES dumpbin HINTS "${_linker_dir}" REQUIRED)
        set(ARN_CODE_PE_TOOL dumpbin)
    else()
        get_filename_component(_objdump_dir "${CMAKE_OBJDUMP}" DIRECTORY)
        find_program(ARN_CODE_PE_SCANNER NAMES objdump HINTS "${_objdump_dir}" REQUIRED)
        set(ARN_CODE_PE_TOOL objdump)
    endif()
    configure_file(cmake/DeployRuntime.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/DeployRuntime.configured.cmake" @ONLY)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/DeployRuntime-$<CONFIG>.cmake"
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/DeployRuntime.configured.cmake")
    # Make every executable launchable as soon as its own target finishes.
    # The ALL target below remains useful when dependencies change or a deployed
    # DLL is removed without forcing the executable to relink.
    foreach(_target IN LISTS _arn_code_runtime_targets)
        add_custom_command(TARGET ${_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                "-DARN_CODE_RUNTIME_EXECUTABLE=$<TARGET_FILE:${_target}>"
                -P "${CMAKE_CURRENT_BINARY_DIR}/DeployRuntime-$<CONFIG>.cmake"
            COMMENT "Deploying runtime dependencies for ${_target}"
            VERBATIM)
    endforeach()
    add_custom_target(arn_code_runtime ALL
        COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/DeployRuntime-$<CONFIG>.cmake"
        DEPENDS ${_arn_code_runtime_targets}
        COMMENT "Deploying runtime dependencies for ARN Code executables"
        VERBATIM)
endif()
