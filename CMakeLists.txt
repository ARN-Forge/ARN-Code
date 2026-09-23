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

add_subdirectory(arn_core)

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
    target_compile_definitions(architecture_test PRIVATE REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(architecture_test PRIVATE arn::core)
    add_test(NAME architecture_boundary COMMAND architecture_test)

    if (MSVC)
        target_compile_options(confirmation_test PRIVATE /utf-8)
        target_compile_options(arn_server_fixture PRIVATE /utf-8)
        target_compile_options(architecture_test PRIVATE /utf-8)
    endif()
endif()
