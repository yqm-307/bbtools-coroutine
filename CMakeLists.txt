cmake_minimum_required(VERSION 3.16)

project(bbtools-coroutine)

# Issue #12 P2：core 直接依赖闭包已内置（bbt/core 子集 + bbt/pollevent），
# 不再外链 libbbt_core。Boost 1.90 为统一基线；context 提供 fcontext。
find_package(Boost 1.90 REQUIRED COMPONENTS context)
find_package(Threads REQUIRED)

################################################################
############## options
################################################################
string(TOUPPER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_UPPER)

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

option(NEED_VALGRIND
    "内存检测专用：把自定义协程栈区间接入 Valgrind（默认OFF，生产构建不依赖Valgrind）" OFF)

message(
    "RELEASE: "         ${BUILD_TYPE_UPPER}
    "\nNEED_BENCHMARK: "  ${NEED_BENCHMARK}
    "\nNEED_EXAMPLE: "    ${NEED_EXAMPLE}
    "\nNEED_TEST: "       ${NEED_TEST}
    "\nNEED_DEBUG: "      ${NEED_DEBUG}
    "\nPROFILE: "         ${PROFILE}
    "\nDEBUG_INFO: "      ${DEBUG_INFO}
    "\nNEED_VALGRIND: "   ${NEED_VALGRIND}
)
################################################################

set(Wall_Flag "-Wdelete-non-virtual-dtor -Wdeprecated-copy \
-Waddress -Warray-bounds=1 -Wbool-compare -Wbool-operation \
-Wduplicated-branches -Wduplicated-cond \
-Wchar-subscripts -Wformat -Wint-in-bool-context \
-Wstrict-aliasing -Wstrict-overflow=1 -Wswitch \
-Wtautological-compare -Wtrigraphs -Wuninitialized \
-Wunused-function -Wunused-label \
-Wunused-value -Wunused-variable -Wnonnull-compare \
-Wmaybe-uninitialized -Wmemset-elt-size -Wreturn-type \
-Wreorder")


if (BUILD_TYPE_UPPER STREQUAL "DEBUG")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${Wall_Flag}")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC ${Wall_Flag}")
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

if (NEED_VALGRIND)
    # 内存检测专用开关：只把协程可用栈区间登记给 Valgrind，消除 boost.context
    # 每次切换 SP 被记为 "client switching stacks?" 的假阳性。默认关闭，生产
    # 构建不引入 Valgrind 依赖。头文件缺失时 fail-closed，不静默降级。
    add_definitions(-DBBT_COROUTINE_VALGRIND)
    # 显式指定的 include 根目录优先（-DBBT_VALGRIND_INCLUDE 或环境变量转发），否则自动搜索。
    # 注意：不能先 set(... "" CACHE PATH ...) 再 find_path —— 空串也算“变量已在 cache 中”，
    # find_path 会因此跳过搜索（CMake 3.16 语义：cache 中非 <VAR>-NOTFOUND 即不搜），
    # 标准路径与 CMAKE_INCLUDE_PATH 就全部失效、必然 FATAL。空值必须先 unset cache 再搜。
    set(BBT_VALGRIND_INCLUDE "" CACHE PATH "valgrind/valgrind.h 所在 include 目录（私有解包场景）")
    if (BBT_VALGRIND_INCLUDE)
        if (NOT EXISTS "${BBT_VALGRIND_INCLUDE}/valgrind/valgrind.h")
            message(FATAL_ERROR
                "BBT_VALGRIND_INCLUDE=${BBT_VALGRIND_INCLUDE} 下没有 valgrind/valgrind.h")
        endif()
    else()
        unset(BBT_VALGRIND_INCLUDE CACHE)
        find_path(BBT_VALGRIND_INCLUDE NAMES valgrind/valgrind.h
            DOC "valgrind/valgrind.h 所在 include 目录（私有解包场景，通常无需指定）")
    endif()
    if (NOT BBT_VALGRIND_INCLUDE)
        message(FATAL_ERROR
            "NEED_VALGRIND=ON 但未找到 valgrind/valgrind.h；"
            "请安装 valgrind 开发头，或用 -DBBT_VALGRIND_INCLUDE=<include目录> 指定")
    endif()
    include_directories(${BBT_VALGRIND_INCLUDE})
    message(STATUS "NEED_VALGRIND: ON (${BBT_VALGRIND_INCLUDE})")
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
    Boost::context
    Threads::Threads
    ${CMAKE_DL_LIBS}    # Hook.hpp 在头文件内 dlsym(RTLD_NEXT,...)，dl 使用方是本库
    # tcmalloc
)

target_compile_features(bbt_coroutine
    PUBLIC cxx_std_17
)

# 添加一个函数来隔离构建类型
function(add_subdirectory_with_build_type subdir build_type)
    set(PARENT_BUILD_TYPE ${CMAKE_BUILD_TYPE})
    set(CMAKE_BUILD_TYPE ${build_type})
    add_subdirectory(${subdir})
    set(CMAKE_BUILD_TYPE ${PARENT_BUILD_TYPE} PARENT_SCOPE)
endfunction()

if (NEED_TEST)
    enable_testing()
    add_subdirectory_with_build_type(unit_test "Debug")
endif()

if (NEED_BENCHMARK)
    add_subdirectory_with_build_type(benchmark_test "Debug")
endif()

if (NEED_EXAMPLE)
    add_subdirectory_with_build_type(example "Debug")
endif()

if (NEED_DEBUG)
    add_subdirectory_with_build_type(debug "Debug")
endif()