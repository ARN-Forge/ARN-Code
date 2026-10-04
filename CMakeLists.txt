cmake_minimum_required(VERSION 3.20)

project(ArnAgentCode VERSION 0.7.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_options(/utf-8 /EHsc)
endif()

include(FetchContent)

# cpp-httplib discovers OpenSSL through CMake when this option is enabled.
set(HTTPLIB_REQUIRE_OPENSSL ON CACHE BOOL "Build cpp-httplib with OpenSSL support" FORCE)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(nlohmann_json cpp_httplib)

find_package(Threads REQUIRED)

include(CTest)
option(ARN_ENABLE_KIRO_INTEGRATION_TESTS "Run tests using an installed/authenticated Kiro CLI" OFF)

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
        set(ARN_CORE_GIT_REVISION "4abac2a09f1ff887640948a55ee21c4f1b6c7ecd")
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

add_executable(arn
    src/main.cpp
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

target_include_directories(arn PRIVATE include)
target_compile_definitions(arn PRIVATE ARN_VERSION="${PROJECT_VERSION}")
target_link_libraries(arn PRIVATE arn::core nlohmann_json::nlohmann_json Threads::Threads)
if (MSVC)
    target_compile_options(arn PRIVATE /utf-8)
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
    endif()
    if (WIN32)
        find_program(ARN_POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
        add_test(NAME windows_install
            COMMAND ${ARN_POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_windows_test.ps1
                    -BuildDir $<TARGET_FILE_DIR:arn>)
        set_tests_properties(windows_install PROPERTIES TIMEOUT 45)
    endif()
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
    if (WIN32)
        add_executable(terminal_windows_test tests/terminal_windows_test.cpp
            src/terminal_windows.cpp src/cli_support.cpp)
        target_include_directories(terminal_windows_test PRIVATE include)
        target_link_libraries(terminal_windows_test PRIVATE Threads::Threads)
        add_test(NAME terminal_windows_input COMMAND terminal_windows_test $<TARGET_FILE:arn>)
        set_tests_properties(terminal_windows_input PROPERTIES TIMEOUT 30)
    endif()
    add_executable(normal_chat_test tests/normal_chat_test.cpp src/normal_chat.cpp)
    target_include_directories(normal_chat_test PRIVATE include)
    target_link_libraries(normal_chat_test PRIVATE arn::core)
    add_test(NAME normal_chat COMMAND normal_chat_test)

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
            model_list_test model_provider_test confirmation_test
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
