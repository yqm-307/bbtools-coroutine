cmake_minimum_required(VERSION 3.1)

project(bbtools-coroutine)

option(RELEASE          "开启将开启优化并不生成debug信息，否则生成debug版本" OFF)
option(NEED_BENCHMARK   "开启将生成Benchmark文件"        ON)
option(NEED_EXAMPLE     "开启将生成example"              ON)              
option(NEED_TEST        "开启将生成test文件"             ON)     
option(NEED_DEBUG       "开启将生成debug文件"            ON)
option(PROFILE          "开启将启用profile"              OFF)

if (RELEASE)
    set(CMAKE_CXX_FALGS "${CMAKE_CXX_FLAGS} -std=c++17 -O2")
else()
    set(CMAKE_CXX_FALGS "${CMAKE_CXX_FLAGS} -std=c++17 -g")
endif()

if (PROFILE)
    add_definitions(-DBBT_COROUTINE_PROFILE)
endif()


set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib) 
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
include_directories(
    ${PROJECT_SOURCE_DIR}
)

file(GLOB_RECURSE SRCS 
    ${PROJECT_SOURCE_DIR}/bbt/*.cc
)

# 创建动态库
add_library(bbt_coroutine SHARED ${SRCS})
target_link_libraries(bbt_coroutine
    boost_context
    event_core
    event_pthreads
    bbt_pollevent
    ybbt
    # tcmalloc
)

## 创建debug动态库
# add_definitions(-DBBT_COROUTINE_ENABLE_DEBUG_PRINT)
add_library(bbt_coroutine_dbg SHARED ${SRCS})
target_link_libraries(bbt_coroutine_dbg
    boost_context
    event_core
    event_pthreads
    bbt_pollevent
    ybbt
)

enable_testing()

if (NEED_TEST)
    add_subdirectory(unit_test)
endif()

if (NEED_BENCHMARK)
    add_subdirectory(benchmark_test)
endif()

if (NEED_EXAMPLE)
    add_subdirectory(example)
endif()

if (NEED_DEBUG)
    add_subdirectory(debug)
endif()