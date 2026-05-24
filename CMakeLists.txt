cmake_minimum_required(VERSION 3.16)
project(hsm VERSION 0.2.0)

# Workaround for doctest FetchContent compatibility
set(CMAKE_POLICY_DEFAULT_CMP0048 NEW)
cmake_policy(SET CMP0048 NEW)
cmake_policy(SET CMP0003 NEW)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_MAKEFILE ON)

# Add compiler-specific flags
if(MSVC)
    add_compile_options(/W4 /WX /GR- /std:c++20 /permissive- /Zc:preprocessor /Zc:__cplusplus /EHs-c-)
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Werror                      # Treat warnings as errors
        -Wconversion
        -Wsign-conversion
        -Wcast-qual
        -Wcast-align
        -Wformat=2
        -Wundef
        -Wunused
        -Wnull-dereference
        -Wdouble-promotion
        -Wold-style-cast
        -Woverloaded-virtual
        -Wnon-virtual-dtor
        -Wshadow
        -Wfloat-equal
        -Wextra-semi
        -Wmissing-field-initializers
        -fno-rtti
        -fno-exceptions              # No runtime exceptions support
        -std=c++20
    )
endif()

include(FetchContent)

FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        PATCH_COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DoctestCMakeLists.txt
            <SOURCE_DIR>/CMakeLists.txt
)
FetchContent_MakeAvailable(doctest)

# Add header-only hsm library
add_library(hsm INTERFACE)
target_include_directories(hsm INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${doctest_SOURCE_DIR}
)

# Define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS for -fno-exceptions compatibility
target_compile_definitions(hsm INTERFACE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

# Add tests if building with tests enabled
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)

    # Optional coverage helper target for the hsm test suite.
    # This relies on the existing coverage flags configured in tests/CMakeLists.txt
    # and will, if gcovr is available, generate a line coverage summary for
    # the core hsm headers.
    option(HSM_ENABLE_COVERAGE "Enable coverage reporting for hsm tests" ON)
    if(HSM_ENABLE_COVERAGE)
        find_program(GCOVR_EXECUTABLE gcovr)
        if(GCOVR_EXECUTABLE)
            add_custom_target(hsm_coverage
                COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --timeout 60
                COMMAND ${GCOVR_EXECUTABLE}
                    --root ${CMAKE_SOURCE_DIR}
                    --object-directory ${CMAKE_BINARY_DIR}
                    --filter ${CMAKE_SOURCE_DIR}/include/hsm
                    --txt
                    --print-summary
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Running hsm tests and generating line coverage summary with gcovr"
            )
        else()
            message(STATUS "gcovr not found; hsm_coverage target will only run tests")
            add_custom_target(hsm_coverage
                COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --timeout 60
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Running hsm tests (coverage summary disabled; install gcovr for reports)"
            )
        endif()
    endif()
endif()

# Add benchmarks if building with benchmarks enabled
option(BUILD_BENCH "Build benchmarks" OFF)
if(BUILD_BENCH)
    add_subdirectory(bench)
endif()

# Add examples if building with examples enabled
option(BUILD_EXAMPLES "Build examples" ON)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
