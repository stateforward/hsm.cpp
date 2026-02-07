# Find all test source files
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
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
