cmake_minimum_required(VERSION 3.20)

project(ArnAgentCode VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# ARN Core dependency (v0.1.0)
# Supports:
# 1. Local development: checks ARN_CORE_SOURCE_DIR or adjacent ../arn-core repository
# 2. Installed package: find_package(arn_core 0.1.0 CONFIG QUIET)
# 3. Remote/CI fallback: FetchContent from https://github.com/arnecto/arn-core.git @ v0.1.0

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
        message(STATUS "Fetching arn_core v0.1.0 from https://github.com/arnecto/arn-core.git")
        FetchContent_Declare(
            arn_core
            GIT_REPOSITORY https://github.com/arnecto/arn-core.git
            GIT_TAG v0.1.0
            GIT_SHALLOW TRUE
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
    src/api_client.cpp
    src/config_manager.cpp
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
endif()

if (BUILD_TESTING)
    add_executable(confirmation_test tests/confirmation_test.cpp
                                     src/tools/workspace_sandbox.cpp
                                     src/tools/coding_tools.cpp)
    target_include_directories(confirmation_test PRIVATE include)
    target_link_libraries(confirmation_test PRIVATE arn::core nlohmann_json::nlohmann_json Threads::Threads)
    add_test(NAME confirmations_and_tools COMMAND confirmation_test)
    add_executable(arn_server_fixture tests/server_fixture.cpp
                                      src/server.cpp
                                      src/tools/workspace_sandbox.cpp
                                      src/tools/coding_tools.cpp)
    target_include_directories(arn_server_fixture PRIVATE include)
    target_link_libraries(arn_server_fixture PRIVATE arn::core nlohmann_json::nlohmann_json Threads::Threads)
    find_package(Python3 COMPONENTS Interpreter)
    if (Python3_FOUND)
        add_test(NAME server_protocol COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/server_protocol_test.py $<TARGET_FILE:arn_server_fixture>)
        set_tests_properties(server_protocol PROPERTIES TIMEOUT 30)
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

    if (MSVC)
        target_compile_options(confirmation_test PRIVATE /utf-8)
        target_compile_options(arn_server_fixture PRIVATE /utf-8)
        target_compile_options(architecture_test PRIVATE /utf-8)
    endif()
endif()
