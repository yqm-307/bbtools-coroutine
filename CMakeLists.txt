cmake_minimum_required(VERSION 3.1)

project(bbtools-coroutine)

################################################################
############## options
################################################################
option(RELEASE
    "启用编译优化" OFF)

option(NEED_BENCHMARK
    "编译benchmark文件" OFF)

option(NEED_EXAMPLE
    "编译example文件" OFF)

option(NEED_TEST
    "编译unit test文件" OFF)

option(NEED_DEBUG
    "编译debug文件" OFF)

option(PROFILE
    "开启将启用profile" OFF)

option(DEBUG_INFO
    "启用debug info，输出库debug信息" OFF)

option(STRINGENT_DEBUG
    "启动严格dbg模式，带来性能开销极大" OFF)

message(
    "RELEASE: "         ${RELEASE}
    "\nNEED_BENCHMARK: "  ${NEED_BENCHMARK}
    "\nNEED_EXAMPLE: "    ${NEED_EXAMPLE}
    "\nNEED_TEST: "       ${NEED_TEST}
    "\nNEED_DEBUG: "      ${NEED_DEBUG}
    "\nPROFILE: "         ${PROFILE}
    "\nDEBUG_INFO: "      ${DEBUG_INFO}
)
################################################################

set(Wall_Flag "-Wnon-virtual-dtor -Wdelete-non-virtual-dtor -Wdeprecated-copy \
-Waddress -Warray-bounds=1 -Wbool-compare -Wbool-operation \
-Wduplicated-branches -Wduplicated-cond \
-Wchar-subscripts -Wformat -Wint-in-bool-context \
-Wstrict-aliasing -Wstrict-overflow=1 -Wswitch \
-Wtautological-compare -Wtrigraphs -Wuninitialized \
-Wunused-function -Wunused-label \
-Wunused-value -Wunused-variable -Wnonnull-compare \
-Wmaybe-uninitialized -Wmemset-elt-size -Wreturn-type \
-Wreorder")

if (RELEASE)
    set(CMAKE_CXX_FALGS "${CMAKE_CXX_FLAGS} -fPIC -std=c++17 -O3 ${Wall_Flag}")
else()
    set(CMAKE_CXX_FALGS "${CMAKE_CXX_FLAGS} -std=c++17 -g ${Wall_Flag}")
endif()

if (PROFILE)
    add_definitions(-DBBT_COROUTINE_PROFILE)
endif()

if (DEBUG_INFO)
    add_definitions(-DBBT_COROUTINE_DEBUG_PRINT)
endif()

if (STRINGENT_DEBUG)
    add_definitions(-DBBT_COROUTINE_STRINGENT_DEBUG)
endif()

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
    event_core
    event_pthreads
    bbt_pollevent
    bbt_core
    # tcmalloc
)

if (NEED_TEST)
    enable_testing()
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