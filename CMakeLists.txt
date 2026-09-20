cmake_minimum_required(VERSION 3.20)

project(ArnAgentCode VERSION 0.4.0 LANGUAGES CXX)

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

if (WIN32)
    set(ARN_TERMINAL_SOURCE src/terminal_windows.cpp)
else()
    set(ARN_TERMINAL_SOURCE src/terminal_posix.cpp)
endif()

add_executable(arn
    src/main.cpp
    src/api_client.cpp
    src/config_manager.cpp
    src/tool_executor.cpp
    ${ARN_TERMINAL_SOURCE}
)

target_include_directories(arn PRIVATE include)
target_compile_definitions(arn PRIVATE ARN_VERSION="${PROJECT_VERSION}")
target_link_libraries(arn PRIVATE nlohmann_json::nlohmann_json httplib::httplib Threads::Threads)
if (MSVC)
    target_compile_options(arn PRIVATE /utf-8)
endif()
