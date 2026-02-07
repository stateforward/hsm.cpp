# Benchmarks for hsm

cmake_minimum_required(VERSION 3.16)

# Options controlling which external HSM libraries are enabled in the hsm_bench
# executable. Some default to ON so that, by default, we attempt to benchmark
# all supported backends whose dependencies and toolchains are compatible with
# the current environment.
#
# hsmcpp is currently disabled by default because its upstream implementation
# relies on std::shared_ptr::unique(), which is not available on all standard
# library implementations used for hsm. You can re-enable it explicitly with
# -DHSM_BENCH_ENABLE_HSMCPP=ON if your toolchain supports it.
option(HSM_BENCH_ENABLE_HSMCPP "Enable hsmcpp backend in hsm_bench" OFF)
option(HSM_BENCH_ENABLE_BOOST_MSM "Enable Boost.MSM backend in hsm_bench" ON)
option(HSM_BENCH_ENABLE_QP "Enable QP/C++ backend in hsm_bench" ON)
option(HSM_BENCH_ENABLE_SML "Enable Boost-ext.SML backend in hsm_bench" ON)
option(HSM_BENCH_ENABLE_HFSM2 "Enable HFSM2 backend in hsm_bench" ON)

set(HSM_BENCH_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/hsm_bench.cpp
)

find_library(GPERFTOOLS_PROFILER profiler)
find_library(GPERFTOOLS_TCMALLOC tcmalloc)
find_path(GPERFTOOLS_INCLUDE_DIR gperftools/profiler.h)

add_executable(hsm_bench ${HSM_BENCH_SOURCES})

# Core dependencies: hsm itself plus doctest (via FetchContent in the
# top-level CMake). gperftools is optional but linked when found to make
# profiling easier in local environments.
target_include_directories(hsm_bench PRIVATE ${GPERFTOOLS_INCLUDE_DIR})

# Link against hsm, doctest, and optional gperftools libraries if found.
# If gperftools is not available, the corresponding variables will be empty
# and CMake will simply omit them from the link line.
target_link_libraries(hsm_bench PRIVATE hsm doctest ${GPERFTOOLS_PROFILER} ${GPERFTOOLS_TCMALLOC})

# Convenience custom target: "bench" will build and then run hsm_bench.
# Arguments can be configured via the HSM_BENCH_ARGS cache variable, which
# should contain a CMake list (semicolon-separated) of command-line arguments.
#
# Example usage from the build directory:
#   cmake .. -DHSM_BENCH_ARGS="--format=text;--scenarios=ping_pong"
#   cmake --build . --target bench
#
# If HSM_BENCH_ARGS is empty, hsm_bench runs with its built-in defaults
# (all libraries, all scenarios, high iteration counts).
set(HSM_BENCH_ARGS "" CACHE STRING "Arguments to pass to hsm_bench when using the 'bench' custom target")

add_custom_target(bench
    COMMAND $<TARGET_FILE:hsm_bench> ${HSM_BENCH_ARGS}
    DEPENDS hsm_bench
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running hsm_bench ${HSM_BENCH_ARGS}"
)

# For the unified HSM comparison benchmark, expose per-backend flags so the
# C++ code can conditionally compile adapters for external libraries. The
# default is to build all backends whose dependencies are present; disabling a
# backend here simply omits its adapter code.

# Third-party headers currently trigger warnings that are treated as errors by
# our strict flags. Disable -Werror for this benchmark target so that warnings
# from external libraries do not break the build, while core hsm code and
# tests still compile with -Werror.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU|AppleClang")
    target_compile_options(hsm_bench PRIVATE
        -Wno-error
        -Wno-error=extra-semi
        -Wno-error=old-style-cast
        -Wno-error=cast-qual
        -Wno-error=non-virtual-dtor
        -Wno-error=implicit-int-conversion
        -Wno-error=undef
        -Wno-error=pessimizing-move
        -Wno-error=shorten-64-to-32)
endif()

include(FetchContent)

if(HSM_BENCH_ENABLE_HSMCPP)
    target_compile_definitions(hsm_bench PRIVATE HSM_BENCH_ENABLE_HSMCPP=1)

    # Fetch hsmcpp when benchmarks for it are enabled. We rely only on the
    # headers for the benchmark; building and linking the full library is left
    # to consumers that need it.
    FetchContent_Declare(
        hsmcpp
        GIT_REPOSITORY https://github.com/igor-krechetov/hsmcpp.git
        GIT_TAG main
    )
    FetchContent_MakeAvailable(hsmcpp)
    target_include_directories(hsm_bench PRIVATE ${hsmcpp_SOURCE_DIR}/include)
    target_link_libraries(hsm_bench PRIVATE hsmcpp)
    target_sources(hsm_bench PRIVATE
        ${hsmcpp_SOURCE_DIR}/src/HsmEventDispatcherSTD.cpp
        ${hsmcpp_SOURCE_DIR}/src/os/stl/ConditionVariable.cpp
    )
endif()

if(HSM_BENCH_ENABLE_BOOST_MSM)
    target_compile_definitions(hsm_bench PRIVATE HSM_BENCH_ENABLE_BOOST_MSM=1)

    # Use an existing Boost installation for Boost.MSM. Although MSM is
    # header-only, Boost.Exception may still be required for
    # boost::throw_exception. We only need Boost headers here; the benchmark
    # provides a local throw_exception definition when needed for Boost.MSM.
    find_package(Boost QUIET)
    if(Boost_FOUND)
        target_include_directories(hsm_bench PRIVATE ${Boost_INCLUDE_DIRS})
        if(TARGET Boost::boost)
            target_link_libraries(hsm_bench PRIVATE Boost::boost)
        endif()
    else()
        message(STATUS "Boost not found, disabling Boost.MSM backend in hsm_bench")
        target_compile_definitions(hsm_bench PRIVATE HSM_BENCH_ENABLE_BOOST_MSM=0)
    endif()
endif()

if(HSM_BENCH_ENABLE_QP)
    target_compile_definitions(hsm_bench PRIVATE HSM_BENCH_ENABLE_QP=1)

    # Fetch QP/C++ framework headers.
    FetchContent_Declare(
        qpcpp
        GIT_REPOSITORY https://github.com/QuantumLeaps/qpcpp.git
        GIT_TAG v7.3.0
    )
    FetchContent_MakeAvailable(qpcpp)
    target_include_directories(hsm_bench PRIVATE 
        ${qpcpp_SOURCE_DIR}/include
        ${qpcpp_SOURCE_DIR}/ports/posix-qv
    )
    target_sources(hsm_bench PRIVATE
        ${qpcpp_SOURCE_DIR}/src/qf/qep_hsm.cpp
        ${qpcpp_SOURCE_DIR}/src/qf/qep_msm.cpp
    )
endif()

if(HSM_BENCH_ENABLE_SML)
    target_compile_definitions(hsm_bench PRIVATE HSM_BENCH_ENABLE_SML=1)

    # Conditionally fetch Boost-ext.SML when SML benchmarks are enabled.
    FetchContent_Declare(
        boost_ext_sml
        GIT_REPOSITORY https://github.com/boost-ext/sml.git
        GIT_TAG v1.1.8
    )
    FetchContent_MakeAvailable(boost_ext_sml)
    target_include_directories(hsm_bench PRIVATE ${boost_ext_sml_SOURCE_DIR}/include)
endif()

if(HSM_BENCH_ENABLE_HFSM2)
    target_compile_definitions(hsm_bench PRIVATE HSM_BENCH_ENABLE_HFSM2=1)

    # Fetch HFSM2 headers for the HFSM2 backend.
    FetchContent_Declare(
        hfsm2
        GIT_REPOSITORY https://github.com/andrew-gresyk/HFSM2.git
        GIT_TAG master
    )
    FetchContent_MakeAvailable(hfsm2)
    target_include_directories(hsm_bench PRIVATE ${hfsm2_SOURCE_DIR}/include)
endif()
