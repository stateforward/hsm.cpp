# Options controlling which external HSM libraries are enabled in the
# hsm_comparison_bench example. These default to OFF so the benchmark builds
# without any additional dependencies; turn them ON and provide include paths
# as needed when integrating external libraries.
option(ENABLE_HSMCPP_BENCH "Enable hsmcpp backend in hsm_comparison_bench" OFF)
option(ENABLE_BOOST_MSM_BENCH "Enable Boost.MSM backend in hsm_comparison_bench" OFF)
option(ENABLE_QP_BENCH "Enable QP/C++ backend in hsm_comparison_bench" OFF)
option(ENABLE_SML_BENCH "Enable Boost-ext.SML backend in hsm_comparison_bench" OFF)
option(ENABLE_HFSM2_BENCH "Enable HFSM2 backend in hsm_comparison_bench" OFF)

# Find all example source files
file(GLOB EXAMPLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fno-omit-frame-pointer  -std=c++23")

find_library(GPERFTOOLS_PROFILER profiler)
find_library(GPERFTOOLS_TCMALLOC tcmalloc)
find_path(GPERFTOOLS_INCLUDE_DIR gperftools/profiler.h)

# Create an executable for each example file
foreach(EXAMPLE_SOURCE ${EXAMPLE_SOURCES})
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_SOURCE} NAME_WE)
    if(NOT TARGET ${EXAMPLE_NAME})
        add_executable(${EXAMPLE_NAME} ${EXAMPLE_SOURCE})
        target_include_directories(${EXAMPLE_NAME} PRIVATE ${GPERFTOOLS_INCLUDE_DIR})
        target_link_libraries(${EXAMPLE_NAME} PRIVATE hsm doctest ${GPERFTOOLS_PROFILER} ${GPERFTOOLS_TCMALLOC})

        # For the unified HSM comparison benchmark, expose per-backend flags so
        # the C++ code can conditionally compile adapters for external
        # libraries. The default is to build only the hsm and vanilla
        # baselines; enabling additional options fetches the corresponding
        # headers via FetchContent.
        if(EXAMPLE_NAME STREQUAL "hsm_comparison_bench" OR EXAMPLE_NAME STREQUAL "hsm_footprint")
            # Third-party headers currently trigger warnings that are treated as
            # errors by our global -Werror. Relax those specific warnings for
            # the benchmark translation unit only.
            target_compile_options(${EXAMPLE_NAME} PRIVATE
                -Wno-error=extra-semi
                -Wno-error=old-style-cast
                -Wno-error=cast-qual)

            if(ENABLE_HSMCPP_BENCH)
                target_compile_definitions(${EXAMPLE_NAME} PRIVATE HSM_BENCH_ENABLE_HSMCPP=1)

        # Fetch hsmcpp when benchmarks for it are enabled. We rely only
        # on the headers for the benchmark; building and linking the
        # full library is left to consumers that need it.
        include(FetchContent)
                FetchContent_Declare(
                    hsmcpp
                    GIT_REPOSITORY https://github.com/igor-krechetov/hsmcpp.git
                    GIT_TAG main
                )
                FetchContent_MakeAvailable(hsmcpp)
                target_include_directories(${EXAMPLE_NAME} PRIVATE ${hsmcpp_SOURCE_DIR}/include)
                # Link against hsmcpp so that dispatcher and HSM symbols resolve.
                target_link_libraries(${EXAMPLE_NAME} PRIVATE hsmcpp)
                target_sources(${EXAMPLE_NAME} PRIVATE
                    ${hsmcpp_SOURCE_DIR}/src/HsmEventDispatcherSTD.cpp
                    ${hsmcpp_SOURCE_DIR}/src/os/stl/ConditionVariable.cpp
                )
            endif()

            if(ENABLE_BOOST_MSM_BENCH)
                target_compile_definitions(${EXAMPLE_NAME} PRIVATE HSM_BENCH_ENABLE_BOOST_MSM=1)

                # Use an existing Boost installation for Boost.MSM headers. We
                # only need the headers (MSM is header-only), so we avoid
                # fetching and building the full Boost superproject which would
                # inherit the project's strict warning flags.
                find_package(Boost REQUIRED)
                target_include_directories(${EXAMPLE_NAME} PRIVATE ${Boost_INCLUDE_DIRS})
            endif()

            if(ENABLE_QP_BENCH)
                target_compile_definitions(${EXAMPLE_NAME} PRIVATE HSM_BENCH_ENABLE_QP=1)

                # Fetch QP/C++ framework headers. QP is licensed under
                # GPL/commercial terms; this download happens only when
                # ENABLE_QP_BENCH is ON so users can make an explicit choice.
                include(FetchContent)
                FetchContent_Declare(
                    qpcpp
                    GIT_REPOSITORY https://github.com/QuantumLeaps/qpcpp.git
                    GIT_TAG v7.3.0
                )
                FetchContent_MakeAvailable(qpcpp)
                target_include_directories(${EXAMPLE_NAME} PRIVATE 
                    ${qpcpp_SOURCE_DIR}/include
                    ${qpcpp_SOURCE_DIR}/ports/posix-qv
                )
                target_sources(${EXAMPLE_NAME} PRIVATE
                    ${qpcpp_SOURCE_DIR}/src/qf/qep_hsm.cpp
                    ${qpcpp_SOURCE_DIR}/src/qf/qep_msm.cpp
                )
            endif()

            if(ENABLE_SML_BENCH)
                target_compile_definitions(${EXAMPLE_NAME} PRIVATE HSM_BENCH_ENABLE_SML=1)

                # Conditionally fetch Boost-ext.SML when SML benchmarks are
                # enabled. This keeps the default configuration free of
                # external dependencies while allowing users to opt in.
                include(FetchContent)
                FetchContent_Declare(
                    boost_ext_sml
                    GIT_REPOSITORY https://github.com/boost-ext/sml.git
                    GIT_TAG v1.1.8
                )
                FetchContent_MakeAvailable(boost_ext_sml)
                target_include_directories(${EXAMPLE_NAME} PRIVATE ${boost_ext_sml_SOURCE_DIR}/include)
            endif()

            if(ENABLE_HFSM2_BENCH)
                target_compile_definitions(${EXAMPLE_NAME} PRIVATE HSM_BENCH_ENABLE_HFSM2=1)

                # Fetch HFSM2 headers for the HFSM2 backend.
                include(FetchContent)
                FetchContent_Declare(
                    hfsm2
                    GIT_REPOSITORY https://github.com/andrew-gresyk/HFSM2.git
                    GIT_TAG master
                )
                FetchContent_MakeAvailable(hfsm2)
                target_include_directories(${EXAMPLE_NAME} PRIVATE ${hfsm2_SOURCE_DIR}/include)
            endif()
        endif()
    endif()
endforeach()
