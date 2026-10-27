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

option(BBT_COROUTINE_STACK_DIAGNOSTICS
    "可选协程栈采样诊断（GCC -finstrument-functions，仅 Linux x86_64 + GNU，默认OFF）" OFF)

message(
    "RELEASE: "         ${BUILD_TYPE_UPPER}
    "\nNEED_BENCHMARK: "  ${NEED_BENCHMARK}
    "\nNEED_EXAMPLE: "    ${NEED_EXAMPLE}
    "\nNEED_TEST: "       ${NEED_TEST}
    "\nNEED_DEBUG: "      ${NEED_DEBUG}
    "\nPROFILE: "         ${PROFILE}
    "\nDEBUG_INFO: "      ${DEBUG_INFO}
    "\nNEED_VALGRIND: "   ${NEED_VALGRIND}
    "\nSTACK_DIAGNOSTICS: " ${BBT_COROUTINE_STACK_DIAGNOSTICS}
)

# 栈采样诊断只对已实证组合开放；其他平台/编译器 fail-closed，不静默降级。
if (BBT_COROUTINE_STACK_DIAGNOSTICS)
    if (NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux"
            AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
            AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
        message(FATAL_ERROR
            "BBT_COROUTINE_STACK_DIAGNOSTICS 仅支持 Linux x86_64 + GNU "
            "(-finstrument-functions + %rsp 采样)，当前组合不支持")
    endif()
endif()
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

# bbt_co_async/bbt_co_await 的误用只能靠编译器诊断兜底，必须是错误而不是警告：
#   - return-type：bbt_co_async(void) 函数走到末尾不写 `return {};` 是未定义行为；
#   - unused-result：漏写 bbt_co_await 时函数体照常执行，只有 AsyncReturn 的
#     [[nodiscard]] 会报 -Wunused-result。
# 依赖本库的仓库（infra、framework 等）需在自己的 CMake 中同样开启，详见
# bbt/coroutine/syntax/Async.hpp 宏处注释。
add_compile_options(-Werror=return-type -Werror=unused-result)

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

# 诊断 TU 需要专属 -fno-instrument-functions（hook 自插桩会递归爆栈），
# 只在 BBT_COROUTINE_STACK_DIAGNOSTICS=ON 时经 target_sources 单独接入；
# GLOB 中始终排除，保证 OFF 构建产物没有任何 hook/诊断符号。
list(REMOVE_ITEM SRCS
    ${PROJECT_SOURCE_DIR}/bbt/coroutine/detail/debug/StackBudgetDiagnostics.cc)

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

if (BBT_COROUTINE_STACK_DIAGNOSTICS)
    target_sources(bbt_coroutine PRIVATE
        ${PROJECT_SOURCE_DIR}/bbt/coroutine/detail/debug/StackBudgetDiagnostics.cc)
    # 宏 PUBLIC：Context/Coroutine 头内的诊断成员与 API 需要消费 TU 同口径；
    # 插桩只 PRIVATE 作用于库自身 TU，不替用户 target 全局开启——需要观测
    # 业务路径的用户对选定 target 自行加 -finstrument-functions。
    # 独立于 PROFILE：BBT_COROUTINE_PROFILE 不会误启用插桩。
    target_compile_definitions(bbt_coroutine PUBLIC BBT_COROUTINE_STACK_DIAGNOSTICS)
    target_compile_options(bbt_coroutine PRIVATE -finstrument-functions)
    set_source_files_properties(
        ${PROJECT_SOURCE_DIR}/bbt/coroutine/detail/debug/StackBudgetDiagnostics.cc
        PROPERTIES COMPILE_OPTIONS "-fno-instrument-functions")
endif()

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