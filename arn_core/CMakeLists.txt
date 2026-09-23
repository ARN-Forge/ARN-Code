cmake_minimum_required(VERSION 3.20)

project(ArnCore VERSION 0.1.0 LANGUAGES CXX)

add_library(arn_core
    src/version.cpp
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
    PRIVATE
        Threads::Threads
)

if (MSVC)
    target_compile_options(arn_core PRIVATE /utf-8)
endif()
