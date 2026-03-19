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

cmake_minimum_required(VERSION 4.0)

project(async_context LANGUAGES CXX)

# libhal cmake utilities from libhal-cmake-util
# Source: https://github.com/libhal/libhal-cmake-util
find_package(LibhalCMakeUtil REQUIRED)

libhal_project_init()
libhal_add_library(async_context
    MODULES
        modules/async_context.cppm
        modules/schedulers.cppm)
libhal_apply_compile_options(async_context)
libhal_install_library(async_context NAMESPACE libhal)
libhal_add_tests(async_context
    TEST_NAMES
        sync_wait
        basics
        blocked_by
        cancel
        exclusive_access
        proxy
        basics_dep_inject
        on_unblock
        simple_scheduler
        clock_adapter
        run_until_done

    MODULES
        tests/util.cppm
)

# ==============================================================================
# Benchmarking
# ==============================================================================

if(NOT CMAKE_CROSSCOMPILING)
    message(STATUS "Building benchmarks tests!")
    find_package(benchmark REQUIRED)
    libhal_add_executable(benchmark SOURCES benchmarks/benchmark.cpp)
    target_link_libraries(benchmark PRIVATE async_context benchmark::benchmark)
else()
    message(STATUS "Cross compiling, skipping benchmarks")
endif() # if(NOT CMAKE_CROSSCOMPILING)
