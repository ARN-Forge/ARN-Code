cmake_minimum_required(VERSION 3.20)

project(ArnCore VERSION 0.1.0 LANGUAGES CXX)

add_library(arn_core
    src/version.cpp
    src/net/http_client.cpp
    src/net/model_parser.cpp
    src/tool/tool_registry.cpp
)

add_library(arn::core ALIAS arn_core)

target_include_directories(arn_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(arn_core PUBLIC cxx_std_23)

target_link_libraries(arn_core
    PUBLIC
        nlohmann_json::nlohmann_json
        httplib::httplib
        Threads::Threads
)

if (MSVC)
    target_compile_options(arn_core PRIVATE /utf-8)
endif()

if (BUILD_TESTING)
    add_executable(arn_core_sse_test tests/sse_decoder_test.cpp)
    target_link_libraries(arn_core_sse_test PRIVATE arn::core)
    add_test(NAME arn_core_sse COMMAND arn_core_sse_test)

    add_executable(arn_core_retry_test tests/http_retry_test.cpp)
    target_link_libraries(arn_core_retry_test PRIVATE arn::core)
    add_test(NAME arn_core_retry COMMAND arn_core_retry_test)

    add_executable(arn_core_tool_registry_test tests/tool_registry_test.cpp)
    target_link_libraries(arn_core_tool_registry_test PRIVATE arn::core)
    add_test(NAME arn_core_tool_registry COMMAND arn_core_tool_registry_test)

    if (MSVC)
        target_compile_options(arn_core_sse_test PRIVATE /utf-8)
        target_compile_options(arn_core_retry_test PRIVATE /utf-8)
        target_compile_options(arn_core_tool_registry_test PRIVATE /utf-8)
    endif()
endif()
