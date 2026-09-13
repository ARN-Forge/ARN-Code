cmake_minimum_required(VERSION 3.30)

project(ArnAgentCode VERSION 0.1.0 LANGUAGES CXX)

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

add_executable(arn
    src/main.cpp
    src/api_client.cpp
    src/config_manager.cpp
    src/tool_executor.cpp
)

target_include_directories(arn PRIVATE include)
target_link_libraries(arn PRIVATE nlohmann_json::nlohmann_json httplib::httplib)
if (MSVC)
    target_compile_options(arn PRIVATE /utf-8)
endif()
