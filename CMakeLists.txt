cmake_minimum_required(VERSION 3.1)

project(bbtools-coroutine)

# set(CMAKE_CXX_FALGS "${CMAKE_CXX_FLAGS} -std=c++17 -g -O2")
set(CMAKE_CXX_FALGS "${CMAKE_CXX_FLAGS} -std=c++17 -g")


set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib) 
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
include_directories(
    ${PROJECT_SOURCE_DIR}
)

file(GLOB_RECURSE SRCS 
    ${PROJECT_SOURCE_DIR}/bbt/*.cc
)

add_library(bbt_coroutine SHARED ${SRCS})

target_link_libraries(bbt_coroutine
    boost_context
    ybbt
)

add_subdirectory(unit_test)
enable_testing()