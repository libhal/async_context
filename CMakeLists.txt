# Copyright 2024 - 2025 Khalil Estell and the libhal contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.28)

# Generate compile commands for anyone using our libraries.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

project(async_context LANGUAGES CXX)

# ==============================================================================
# Library
# ==============================================================================

find_package(strong_ptr REQUIRED)

add_library(async_context STATIC)
target_compile_features(async_context PUBLIC cxx_std_23)
target_link_libraries(async_context PUBLIC strong_ptr)
target_sources(async_context PUBLIC
    FILE_SET CXX_MODULES
    TYPE CXX_MODULES

    FILES
    modules/async_context.cppm
)
target_compile_options(async_context PRIVATE
    -g
    -Werror
    -Wno-unused-command-line-argument
    -Wall
    -Wextra
    -Wshadow
    -fexceptions
    -fno-rtti)

install(
    TARGETS async_context
    EXPORT async_context_targets
    FILE_SET CXX_MODULES DESTINATION "."
    LIBRARY DESTINATION "lib"
    ARCHIVE DESTINATION "lib"
    CXX_MODULES_BMI DESTINATION "bmi"
)

install(
    EXPORT async_context_targets
    FILE "async_context-config.cmake"
    DESTINATION "lib/cmake"
    CXX_MODULES_DIRECTORY "cxx-modules"
    EXPORT_PACKAGE_DEPENDENCIES
)

# ==============================================================================
# Unit testing
# ==============================================================================

find_package(ut REQUIRED)

if(CMAKE_CROSSCOMPILING)
    message(STATUS "Cross compiling, skipping unit test execution")
else()
    message(STATUS "Building unit tests!")
    add_executable(async_unit_test)
    # Add module files
    target_sources(async_unit_test
    PRIVATE
        tests/main.test.cpp
        tests/async.test.cpp
    )

    target_compile_features(async_unit_test PUBLIC cxx_std_23)
    target_compile_options(async_unit_test PRIVATE
        -g
        -Werror
        -Wno-unused-command-line-argument
        -Wall
        -Wextra
        -Wshadow
        -fexceptions
        -fno-rtti)
    target_link_libraries(async_unit_test PRIVATE async_context Boost::ut)

    add_custom_target(run_tests ALL DEPENDS async_unit_test COMMAND async_unit_test)
endif()

# Always run this custom target by making it depend on ALL
add_custom_target(copy_compile_commands ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_BINARY_DIR}/compile_commands.json
    ${CMAKE_SOURCE_DIR}/compile_commands.json
    DEPENDS ${CMAKE_BINARY_DIR}/compile_commands.json)
