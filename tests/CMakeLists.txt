# Find all test source files
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB REPORTED_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/reported/*.cpp")
list(APPEND TEST_SOURCES ${REPORTED_SOURCES})
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} --coverage")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")

find_library(GPERFTOOLS_PROFILER profiler)
find_path(GPERFTOOLS_INCLUDE_DIR gperftools/profiler.h)

# Create an executable for each test file
foreach(TEST_SOURCE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_include_directories(${TEST_NAME} PRIVATE ${GPERFTOOLS_INCLUDE_DIR})

    # Link hsm library
    target_link_libraries(${TEST_NAME} PRIVATE hsm doctest ${GPERFTOOLS_PROFILER})

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# Compile-fail tests: each should fail to compile.
# CTest WILL_FAIL means the test passes when the build command fails.
file(GLOB COMPILE_FAIL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/*.cpp")
foreach(src ${COMPILE_FAIL_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_library(compile_fail_${name} STATIC EXCLUDE_FROM_ALL ${src})
    target_link_libraries(compile_fail_${name} PRIVATE hsm)
    add_test(NAME compile_fail_${name}
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                --target compile_fail_${name} --config $<CONFIG>)
    set_tests_properties(compile_fail_${name} PROPERTIES WILL_FAIL TRUE)
endforeach()
