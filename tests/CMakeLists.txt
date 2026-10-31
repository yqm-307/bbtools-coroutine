# 测试约定见 docs/conventions.md §6：文件 Test_<行为>.cc，ctest 名
# <module>.<行为>（行为段小写下划线），label ∈ {smoke,unit,integration,live,perf}。
# 本目录按模块分子目录：core/ coroutine/ pollevent/。
#
# helper bbt_unit_test(<target> [ctest 名后缀...])
#   - 源文件固定为 <模块目录>/<target>.cc；target 名与文件名同名。
#   - 无后缀参数时注册 <module>.<target 去 Test_ 前缀>；
#     有后缀参数时逐条注册 <module>.<target 去前缀>_<mode>，
#     并将 MODE_ARG_<mode> 设为其参数（默认 = mode 本身）。
#   - LABELS "<labels>" 应用于该 target 注册的每条 ctest；
#     LABELS_<mode> "<labels>" 只覆盖该条。
#   - TIMEOUT <n> 为默认值，TIMEOUT_<mode> 覆盖单条。
set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti ${Wall_Flag}")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin/tests)

# 测试文件通过 BOOST_TEST_MAIN + boost/test/included/unit_test.hpp 静态内嵌 Boost.Test，
# 不得再链接 libboost_unit_test_framework.so：共享库与二进制各持一份框架全局状态，
# ELF 符号插入使 .so 析构释放二进制已释放的对象，__cxa_finalize 阶段 double-free。
set(MY_LIBS bbt_coroutine)

function(_bbt_ut_module_of target out_var)
    foreach(mod core coroutine pollevent)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${mod}/${target}.cc")
            set(${out_var} ${mod} PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "bbt_unit_test: 找不到 ${target}.cc 所属模块目录")
endfunction()

# bbt_unit_test(<target>
#               [MODES m1 m2 ...]           缺省时注册单条 <module>.<name>
#               [MODE_ARGS m1 arg1 ...]     以 "m1:" 前缀的参数对表（值含空格用引号）
#               [COMMAND_ARG <arg>]         单条模式的命令参数
#               [TIMEOUT <n>] [TIMEOUT_<m> <n> ...]
#               [LABELS "<l>"]  [LABELS_<m> "<l>" ...]
#               [RUN_SERIAL]  [RUN_SERIAL_<m>]
#               [EXTRA_LIBS <libs>...] [EXTRA_LINK_OPTIONS <opts>...])
function(bbt_unit_test target)
    _bbt_ut_module_of(${target} _mod)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_mod}/${target}.cc")
    string(REGEX REPLACE "^Test_" "" _base "${target}")

    cmake_parse_arguments(UT "RUN_SERIAL" "TIMEOUT;COMMAND_ARG"
        "MODES;MODE_ARGS;EXTRA_LIBS;EXTRA_LINK_OPTIONS;EXTRA_SOURCES;LABELS" ${ARGN})

    add_executable(${target} ${_src} ${UT_EXTRA_SOURCES})
    target_link_libraries(${target} ${MY_LIBS} ${UT_EXTRA_LIBS})
    if(UT_EXTRA_LINK_OPTIONS)
        target_link_options(${target} PRIVATE ${UT_EXTRA_LINK_OPTIONS})
    endif()

    # 注册：单条（无 MODES）或按 mode 展开
    set(_register_modes ${UT_MODES})
    if(NOT UT_MODES)
        set(_register_modes __single__)
    endif()
    foreach(_mode IN LISTS _register_modes)
        if(_mode STREQUAL "__single__")
            set(_name "${_mod}.${_base}")
            set(_cmd ${target})
            if(UT_COMMAND_ARG)
                list(APPEND _cmd ${UT_COMMAND_ARG})
            endif()
            set(_mode "")
        else()
            set(_name "${_mod}.${_base}_${_mode}")
            set(_cmd ${target})
            # MODE_ARGS 中找 "<mode>:<args>"，缺省时直接把 mode 当参数
            set(_mode_arg_found FALSE)
            foreach(_pair IN LISTS UT_MODE_ARGS)
                if(_pair MATCHES "^${_mode}:(.*)$")
                    separate_arguments(_ma UNIX_COMMAND "${CMAKE_MATCH_1}")
                    list(APPEND _cmd ${_ma})
                    set(_mode_arg_found TRUE)
                endif()
            endforeach()
            if(NOT _mode_arg_found)
                list(APPEND _cmd ${_mode})
            endif()
        endif()
        add_test(NAME ${_name} COMMAND ${_cmd})

        set(_props "")
        if(UT_TIMEOUT)
            list(APPEND _props TIMEOUT ${UT_TIMEOUT})
        endif()
        if(_mode AND DEFINED UT_TIMEOUT_${_mode})
            list(APPEND _props TIMEOUT ${UT_TIMEOUT_${_mode}})
        endif()
        if(UT_RUN_SERIAL OR (_mode AND UT_RUN_SERIAL_${_mode}))
            list(APPEND _props RUN_SERIAL TRUE)
        endif()
        if(_props)
            set_tests_properties(${_name} PROPERTIES ${_props})
        endif()
        # LABELS 单独设置：值内 ';' 不能在 list 展开里存活
        set(_labels "")
        if(UT_LABELS)
            set(_labels "${UT_LABELS}")
        endif()
        if(_mode AND DEFINED UT_LABELS_${_mode})
            set(_labels "${UT_LABELS_${_mode}}")
        endif()
        if(_labels)
            set_tests_properties(${_name} PROPERTIES LABELS "${_labels}")
        endif()
    endforeach()
endfunction()

################################################################
# core 模块
################################################################
bbt_unit_test(Test_countdown_latch TIMEOUT 15 LABELS "unit")

################################################################
# pollevent 模块（pollevent/Event、EventLoop 直测，不依赖协程 runtime）
################################################################
bbt_unit_test(Test_eventloop TIMEOUT 30 LABELS "unit")

# 构造回滚：fd 单次关闭与同号新对象存活。ELF interpose 需 export-dynamic。
bbt_unit_test(Test_event_constructor_rollback
    MODES rollback_timer_fail construct_ok
    TIMEOUT 15 LABELS "unit"
    EXTRA_LINK_OPTIONS "-Wl,--export-dynamic")

# 进程退出时仍存活的 Event 不得遭注册表静态析构，正常取消/析构仍逐项回收。
bbt_unit_test(Test_event_callback_registry_lifetime
    MODES exit_live_event normal_reclaim cancel_reclaim
    TIMEOUT 15 LABELS "unit")

################################################################
# coroutine 模块：runtime / 栈 / 调度
################################################################

# 测试构建专用窄故障注入：唯有本文件（由根 CMakeLists.txt 的
# if(NEED_TEST) 加入，自身没有 NEED_TEST 分支）打开这个诊断宏，PUBLIC 让库自身
# 的 TU 与全部消费 TU 同编同一口径 —— 只定义测试 TU 会让库路径没有注入。
# NEED_TEST=OFF 的生产构建不含该定义：源码 seam/分支与符号全部缺失（已用
# nm 对未 strip 的实际库做正反对照）。
target_compile_definitions(bbt_coroutine PUBLIC BBT_COROUTINE_STACK_TESTING)

bbt_unit_test(Test_runtime_oom
    MODES fresh prepare tracked trigger defer
    TIMEOUT 15 LABELS "unit"
    EXTRA_LINK_OPTIONS "-Wl,--export-dynamic")

# 含真实定时回收等待（m_cfg_stackpool_adjust_interval=5000ms 量级）与隔离子进程
bbt_unit_test(Test_coroutine_stack TIMEOUT 180 LABELS "unit")

# 多尺寸固定栈 runtime：薄 main 三种模式，各自独立进程（单进程只 Start 一次）
bbt_unit_test(Test_multi_size_stack_runtime
    MODES protected unprotected reject_before_start
    MODE_ARGS "protected:run-protected"
              "unprotected:run-unprotected"
              "reject_before_start:reject-before-start"
    TIMEOUT 60 LABELS "unit")

bbt_unit_test(Test_coroutine LABELS "smoke;unit" TIMEOUT 60)

# 无创建探针与 runtime 分属独立进程，避免 worker 创建污染单调 ID 判据。
bbt_unit_test(Test_coroutine_query
    MODES no_create runtime
    MODE_ARGS "no_create:--run_test=query_no_create"
              "runtime:--run_test=query_runtime"
    TIMEOUT 20 LABELS "unit")

bbt_unit_test(Test_coroutine_api LABELS "unit")
bbt_unit_test(Test_async_syntax TIMEOUT 20 LABELS "unit")
bbt_unit_test(Test_coroutine_exception LABELS "smoke;unit" TIMEOUT 60)
bbt_unit_test(Test_coroutine_cancel TIMEOUT 10 LABELS "unit")
bbt_unit_test(Test_exception_stop TIMEOUT 10 LABELS "unit")

# 扩缩容配置/驱动模式各独立进程，禁止一次进程重复 Start。
add_executable(Test_scheduler_scaling coroutine/Test_scheduler_scaling.cc)
# 只为 scaling 可执行生成源副本：四个观察点不替换产品条件/锁/原子序。
# 锚点必须唯一；产品代码漂移立即 configure 失败，禁止静默漏掉交错。
set(_sc_source "${PROJECT_SOURCE_DIR}/bbt/coroutine/detail/Processer.cc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sc_source}")
file(READ "${_sc_source}" _sc_probe)
function(scaling_probe_replace anchor replacement)
    string(REPLACE "${anchor}" "" _removed "${_sc_probe}")
    string(LENGTH "${_sc_probe}" _old_len)
    string(LENGTH "${_removed}" _new_len)
    string(LENGTH "${anchor}" _anchor_len)
    math(EXPR _difference "${_old_len} - ${_new_len}")
    if(NOT _difference EQUAL _anchor_len)
        message(FATAL_ERROR "Scaling probe anchor missing/non-unique: ${anchor}")
    endif()
    string(REPLACE "${anchor}" "${replacement}" _result "${_sc_probe}")
    set(_sc_probe "${_result}" PARENT_SCOPE)
endfunction()
scaling_probe_replace(
    "    m_enqueue_inflight.fetch_add(1, std::memory_order_seq_cst);"
    "    ::ScalingAdmissionProbe(this, 1);\n    m_enqueue_inflight.fetch_add(1, std::memory_order_seq_cst);")
scaling_probe_replace(
    "    const bool enqueued = m_coroutine_queue.Enqueue(coroutine->m_cfs, coroutine, priority,"
    "    ::ScalingAdmissionProbe(this, 2);\n    const bool enqueued = m_coroutine_queue.Enqueue(coroutine->m_cfs, coroutine, priority,")
scaling_probe_replace("    _WaitEnqueueQuiescent();"
    "    ::ScalingAdmissionProbe(this, 3);\n    _WaitEnqueueQuiescent();")
scaling_probe_replace("\n}\n\nbool Processer::_HasDriverOwnership()"
    "\n    ::ScalingAdmissionProbe(this, 4);\n}\n\nbool Processer::_HasDriverOwnership()")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/Processer_scaling_probe.cc"
    "#include <bbt/coroutine/detail/Processer.hpp>\nvoid ScalingAdmissionProbe(bbt::coroutine::detail::Processer*, int);\n${_sc_probe}")
target_sources(Test_scheduler_scaling PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Processer_scaling_probe.cc")
set(_sc_scheduler "${PROJECT_SOURCE_DIR}/bbt/coroutine/detail/Scheduler.cc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sc_scheduler}")
file(READ "${_sc_scheduler}" _sc_probe)
scaling_probe_replace(
    "                    : target + m_scale_step;\n                const auto result = _ApplyTargetLocked(next, notify, lock);"
    "                    : target + m_scale_step;\n                ::ScalingAutoCommitProbe();\n                const auto result = _ApplyTargetLocked(next, notify, lock);")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/Scheduler_scaling_probe.cc"
    "void ScalingAutoCommitProbe();\n${_sc_probe}")
target_sources(Test_scheduler_scaling PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Scheduler_scaling_probe.cc")
target_link_libraries(Test_scheduler_scaling ${MY_LIBS})
target_link_options(Test_scheduler_scaling PRIVATE -Wl,--export-dynamic)
set(_sc_modes boundaries noloop_loop_reaper noloop_manual_reaper driver_parked auto_busy auto_queue
    tls_thread tls_noloop self_retire pin_bound pin_failed pin_detaching outstanding_limit
    concurrent_control auto_reverse admission_late admission_inflight rebalance_short_backlog)
if(PROFILE)
    list(APPEND _sc_modes profile_cycles)
endif()
foreach(mode IN LISTS _sc_modes)
    add_test(NAME coroutine.scheduler_scaling_${mode} COMMAND Test_scheduler_scaling)
    set_tests_properties(coroutine.scheduler_scaling_${mode} PROPERTIES
        ENVIRONMENT "BBT_SCALING_MODE=${mode}" TIMEOUT 30 LABELS "unit")
endforeach()

# ELF new/pthread interpose 隔离在故障可执行；不向生产库增加测试 seam。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(Test_scheduler_scaling_failures coroutine/Test_scheduler_scaling_failures.cc)
    target_link_libraries(Test_scheduler_scaling_failures ${MY_LIBS} ${CMAKE_DL_LIBS})
    target_link_options(Test_scheduler_scaling_failures PRIVATE -Wl,--export-dynamic)
    foreach(mode expand shrink pthread tls_alloc)
        add_test(NAME coroutine.scheduler_scaling_failures_${mode} COMMAND Test_scheduler_scaling_failures ${mode})
        set_tests_properties(coroutine.scheduler_scaling_failures_${mode} PROPERTIES TIMEOUT 60 LABELS "unit")
    endforeach()
endif()

bbt_unit_test(Test_scheduler_exec TIMEOUT 15 LABELS "unit")

bbt_unit_test(Test_cfs
    MODES algorithm_migration_global_debt algorithm_stable_wake_account_and_weight
        algorithm_wait_preserves_debt_without_old_credit
        single_p_external_progress_before_hot_release multi_p_steal_preserves_running_progress
    MODE_ARGS "algorithm_migration_global_debt:--run_test=algorithm_migration_global_debt"
              "algorithm_stable_wake_account_and_weight:--run_test=algorithm_stable_wake_account_and_weight"
              "algorithm_wait_preserves_debt_without_old_credit:--run_test=algorithm_wait_preserves_debt_without_old_credit"
              "single_p_external_progress_before_hot_release:--run_test=single_p_external_progress_before_hot_release"
              "multi_p_steal_preserves_running_progress:--run_test=multi_p_steal_preserves_running_progress"
    TIMEOUT 15 LABELS "unit")

bbt_unit_test(Test_ready_wakeup
    MODES notification_latched_before_wait external_ready_before_sleep_and_parked
          concurrent_external_notifications external_parked_progress
    MODE_ARGS "notification_latched_before_wait:--run_test=notification_latched_before_wait"
              "external_ready_before_sleep_and_parked:--run_test=external_ready_before_sleep_and_parked"
              "concurrent_external_notifications:--run_test=concurrent_external_notifications"
              "external_parked_progress:--run_test=external_parked_progress"
    TIMEOUT 15 LABELS "unit")

# 跨 worker 恢复与用户 TLS 边界回归：含事件唤醒迁移，耐心等待但必须有界。
bbt_unit_test(Test_worker_tls_boundary TIMEOUT 40 LABELS "unit")

################################################################
# coroutine 模块：hook / io / poller
################################################################
bbt_unit_test(Test_hook LABELS "unit")
bbt_unit_test(Test_dns_lifecycle TIMEOUT 10 LABELS "unit")

# hook 行为契约基座：不进 smoke（含 sleep(1) 级真实时序，保留给完整回归）
bbt_unit_test(Test_hook_contract TIMEOUT 60 LABELS "unit")

bbt_unit_test(Test_hook_blocking_fd TIMEOUT 10 LABELS "unit")
bbt_unit_test(Test_coroutine_diag TIMEOUT 15 LABELS "unit")
bbt_unit_test(Test_hook_timeout_flags TIMEOUT 15 LABELS "unit")

# worker stall 检测依赖真实计时阈值，与其他用例串行防干扰。
bbt_unit_test(Test_worker_stall TIMEOUT 30 RUN_SERIAL LABELS "unit")
add_test(NAME coroutine.worker_stall_disabled COMMAND Test_worker_stall -- --stall-warn-ms=0)
set_tests_properties(coroutine.worker_stall_disabled PROPERTIES
    TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit")

bbt_unit_test(Test_crash_diag TIMEOUT 30 LABELS "unit")
bbt_unit_test(Test_hook_error_matrix TIMEOUT 20 LABELS "unit")

# FD epoch 线性化回归：close+reuse 的判别性用例
bbt_unit_test(Test_hook_fd_epoch TIMEOUT 20 LABELS "unit")

# 多 worker close+reuse 必须独立进程：worker 数在首次 Start 时固定，
# 同一可执行内无法从 1 切到 4（一次初始化契约），故单列一个进程。
bbt_unit_test(Test_hook_fd_epoch_multi TIMEOUT 20 LABELS "unit")

bbt_unit_test(Test_poller LABELS "smoke;unit" TIMEOUT 60)
bbt_unit_test(Test_eventloop_boundary TIMEOUT 10 LABELS "unit")
bbt_unit_test(Test_copollevent_state TIMEOUT 30 LABELS "unit")
bbt_unit_test(Test_copoller_deferred_owner TIMEOUT 15 LABELS "unit")
bbt_unit_test(Test_eventloop_backend TIMEOUT 10 LABELS "unit")
bbt_unit_test(Test_eventloop_contract TIMEOUT 10 LABELS "unit")
bbt_unit_test(Test_io_executor TIMEOUT 15 LABELS "unit")

################################################################
# coroutine 模块：同步原语 / 语法 / 杂项
################################################################
bbt_unit_test(Test_cond LABELS "unit")
bbt_unit_test(Test_g_co LABELS "unit")
bbt_unit_test(Test_macro_wrap TIMEOUT 10 LABELS "unit")
bbt_unit_test(Test_scheduler_api TIMEOUT 10 LABELS "unit")
bbt_unit_test(Test_chan LABELS "smoke;unit" TIMEOUT 60)
bbt_unit_test(Test_coselect TIMEOUT 30 LABELS "unit")
bbt_unit_test(Test_comutex LABELS "smoke;unit" TIMEOUT 60)
bbt_unit_test(Test_lockguard LABELS "unit")
bbt_unit_test(Test_rwlock_guard LABELS "unit")
bbt_unit_test(Test_defer LABELS "unit")
bbt_unit_test(Test_profiler LABELS "unit")
bbt_unit_test(Test_co_rwmutex TIMEOUT 30 LABELS "unit")
bbt_unit_test(Test_coevent LABELS "smoke;unit" TIMEOUT 60)
bbt_unit_test(Test_copool LABELS "smoke;unit" TIMEOUT 60)
bbt_unit_test(Test_smoke LABELS "smoke;unit" TIMEOUT 60)
bbt_unit_test(Test_reliability LABELS "integration")

# 对象身份与运行时代际
bbt_unit_test(Test_co_object_identity TIMEOUT 15 LABELS "unit")

# 唤醒原因独立化 + CoWaiter 窄接口单次等待结果收口
bbt_unit_test(Test_cowaiter_outcome TIMEOUT 30 LABELS "unit")

# CoWaiter 组合等待最小稳定入口（单 FD interest + deadline + Notify + 取消首胜）
bbt_unit_test(Test_combined_wait TIMEOUT 30 LABELS "unit")

# 旧超时族接口登记失败回归（固定失败注入 _TestFailNextAwaitRegist）
bbt_unit_test(Test_cowaiter_regist_fail TIMEOUT 30 LABELS "unit")

# 同步锁等待登记失败的锁不变量、进展和返回码回归
bbt_unit_test(Test_sync_wait_regist_fail TIMEOUT 60 LABELS "unit")

# 挂起/唤醒链中诊断分配失败必须有界收口，且 await/锁/等待者保持可复用
bbt_unit_test(Test_diagnostic_wait_oom
    TIMEOUT 60 LABELS "unit"
    EXTRA_LINK_OPTIONS "-Wl,--export-dynamic")

# 诊断模式下真实挂起、失败重入和终态配对
bbt_unit_test(Test_stringent_debug TIMEOUT 30 LABELS "unit")

################################################################
# coroutine process-lifetime 公共迁移（coroutine 隔离实现）
#  - CoEventValue / 带参等待 新公共面
#  - 公共头逐头独立 TU 编译（仅含伞头不能证明每个头自给）
#  - CoEventValue 跨 DSO 类型识别（两个独立产物 + 一个消费方）
################################################################

bbt_unit_test(Test_coeventvalue TIMEOUT 15 LABELS "unit")
bbt_unit_test(Test_cowaiter_payload TIMEOUT 30 LABELS "unit")

# 等待代次载荷隔离：确定性交错需单 worker，单列进程固定 1 worker。
bbt_unit_test(Test_cowaiter_payload_rounds TIMEOUT 30 LABELS "unit")

# 三种 Start 模式 / 并发 Start / 进程 exit：各自独立子进程（fork）验证
bbt_unit_test(Test_scheduler_start_modes TIMEOUT 30 LABELS "unit")

# 驱动模式不能在同一可执行内切换：NO_LOOP + LoopOnce 单列进程
bbt_unit_test(Test_scheduler_noloop TIMEOUT 20 LABELS "unit")

# 跨 DSO：a 构造载荷、b 读取；两侧 traits 依赖公共头的显式登记 ID
add_library(coeventvalue_dso_a SHARED coroutine/coeventvalue_dso_a.cc)
target_link_libraries(coeventvalue_dso_a ${MY_LIBS})
add_library(coeventvalue_dso_b SHARED coroutine/coeventvalue_dso_b.cc)
target_link_libraries(coeventvalue_dso_b ${MY_LIBS})
add_executable(Test_coeventvalue_dso coroutine/Test_coeventvalue_dso.cc)
target_link_libraries(Test_coeventvalue_dso ${MY_LIBS} coeventvalue_dso_a coeventvalue_dso_b)
add_test(NAME coroutine.coeventvalue_dso COMMAND Test_coeventvalue_dso)
set_tests_properties(coroutine.coeventvalue_dso PROPERTIES TIMEOUT 15 LABELS "integration")

# 公共头逐头独立 TU：每个头单独编译一个 TU，证明头自给（不依赖伞头的包含顺序）
set(PUBLIC_HEADER_TU_DIR ${PROJECT_BINARY_DIR}/public_header_tu)
file(MAKE_DIRECTORY ${PUBLIC_HEADER_TU_DIR})
set(BBT_PUBLIC_HEADERS
    bbt/coroutine/sync/WaitTypes.hpp
    bbt/coroutine/sync/CoEventValue.hpp
    bbt/coroutine/sync/CoWaiter.hpp
    bbt/coroutine/sync/CoMutex.hpp
    bbt/coroutine/sync/CoCond.hpp
    bbt/coroutine/sync/CoRWMutex.hpp
    bbt/coroutine/sync/Chan.hpp
    bbt/coroutine/sync/CoSelect.hpp
    bbt/coroutine/object/interface/ICoObject.hpp
    bbt/coroutine/object/CoObject.hpp
    bbt/coroutine/pool/CoPool.hpp
    bbt/coroutine/detail/Scheduler.hpp
    bbt/coroutine/coroutine.hpp
)
set(BBT_PUBLIC_HEADER_TUS "")
foreach(_hdr ${BBT_PUBLIC_HEADERS})
    string(REPLACE "/" "_" _tu_name ${_hdr})
    string(REPLACE ".hpp" "" _tu_name ${_tu_name})
    set(_tu_file ${PUBLIC_HEADER_TU_DIR}/tu_${_tu_name}.cc)
    file(WRITE ${_tu_file} "#include <${_hdr}>\n")
    list(APPEND BBT_PUBLIC_HEADER_TUS ${_tu_file})
endforeach()
add_library(bbt_public_header_tu OBJECT ${BBT_PUBLIC_HEADER_TUS})
target_link_libraries(bbt_public_header_tu PRIVATE ${MY_LIBS})

# 负向编译检查：超过 24B 的载荷与越界自定义登记 ID 必须编译失败，且失败
# 原因必须是预期的那条断言（不能是命名空间/拼写等无关错误）。
set(BBT_NEGATIVE_DIR ${PROJECT_BINARY_DIR}/negative_compile)
file(MAKE_DIRECTORY ${BBT_NEGATIVE_DIR})

# 正向对照：同款 include + 合法 24B 载荷 + 合法自定义登记 ID 必须通过编译，
# 证明后续负例的失败只来自被检查的断言，而非公共头/命名空间本身。
file(WRITE ${BBT_NEGATIVE_DIR}/ok_24b.cc
    "#include <bbt/coroutine/sync/CoEventValue.hpp>\nstruct OkPayload { unsigned char b[24]; };\nBBT_COEVENT_VALUE_REGISTER_TYPE(OkPayload, 0x100);\nint main() { auto v = bbt::coroutine::CoEventValue::From(OkPayload{}); (void)v; return 0; }\n")

# 容量负例：类型已合法登记（ID 合法），唯一可能的编译错误就是 24B 容量断言。
file(WRITE ${BBT_NEGATIVE_DIR}/too_large.cc
    "#include <bbt/coroutine/sync/CoEventValue.hpp>\nstruct Oversize { unsigned char b[25]; };\nBBT_COEVENT_VALUE_REGISTER_TYPE(Oversize, 0x1ff);\nint main() { auto v = bbt::coroutine::CoEventValue::From(Oversize{}); (void)v; return 0; }\n")
# ID 负例：登记 ID=1 越出自定义区间，须命中区间断言。
file(WRITE ${BBT_NEGATIVE_DIR}/bad_id.cc
    "#include <bbt/coroutine/sync/CoEventValue.hpp>\nstruct BadId { int a; };\nBBT_COEVENT_VALUE_REGISTER_TYPE(BadId, 1);\nint main() { return (int)bbt::coroutine::CoEventValueTraits<BadId>::id; }\n")

# 指针层级负例：超过 kCoEventValueMaxPointerDepth 的层级 ID 会 uint32 回绕并
# 与低位类型撞号，必须由 traits 的 static_assert 拒绝（而非静默回绕）。
# 正向对照：合法深度见 ok_24b.cc 与 Test_coeventvalue 的 t_pointer_depth_within_capacity。
file(WRITE ${BBT_NEGATIVE_DIR}/too_deep.cc
    "#include <bbt/coroutine/sync/CoEventValue.hpp>\nusing P1 = int*; using P2 = P1*; using P3 = P2*; using P4 = P3*;\nusing P5 = P4*; using P6 = P5*; using P7 = P6*; using P8 = P7*;\nint main() { return (int)bbt::coroutine::CoEventValueTraits<P8>::id; }\n")

try_compile(_bbt_pos_compiled
    ${PROJECT_BINARY_DIR}/negative_compile/ok_24b
    ${BBT_NEGATIVE_DIR}/ok_24b.cc
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}"
    CXX_STANDARD 17
    OUTPUT_VARIABLE _bbt_pos_out)
if(NOT _bbt_pos_compiled)
    message(FATAL_ERROR "正向对照编译失败：ok_24b.cc 本应通过：\n${_bbt_pos_out}")
endif()

foreach(_neg too_large bad_id too_deep)
    try_compile(_bbt_neg_compiled
        ${PROJECT_BINARY_DIR}/negative_compile/${_neg}
        ${BBT_NEGATIVE_DIR}/${_neg}.cc
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}"
        CXX_STANDARD 17
        OUTPUT_VARIABLE _bbt_neg_out)
    if(_bbt_neg_compiled)
        message(FATAL_ERROR "负向编译检查失败：${_neg}.cc 本应被拒绝却编译通过")
    endif()
    set(_bbt_neg_out_${_neg} "${_bbt_neg_out}")
endforeach()

if(NOT _bbt_neg_out_too_large MATCHES "载荷超过 24 字节内联容量")
    message(FATAL_ERROR "too_large.cc 的失败原因不是容量断言：\n${_bbt_neg_out_too_large}")
endif()
if(NOT _bbt_neg_out_bad_id MATCHES "自定义类型 ID 必须落在自定义登记区间")
    message(FATAL_ERROR "bad_id.cc 的失败原因不是 ID 区间断言：\n${_bbt_neg_out_bad_id}")
endif()
if(NOT _bbt_neg_out_too_deep MATCHES "指针层级超过 kCoEventValueMaxPointerDepth")
    message(FATAL_ERROR "too_deep.cc 的失败原因不是层级容量断言：\n${_bbt_neg_out_too_deep}")
endif()

# processer driver adapter：每模式独立进程（runtime 只 Start 一次），
# 模式经环境变量传入；timeout 有界但不改任何既有测试预算。
add_executable(Test_processer_driver coroutine/Test_processer_driver.cc)
target_link_libraries(Test_processer_driver ${MY_LIBS})

# 测试专用副本：产品源码/库没有测试分支或 hook。两个精确观察点只用于
# 固定旧 joiner 与新绑定交错、计数实际 continue；不替换任何条件或锁操作。
set(_pd_source "${PROJECT_SOURCE_DIR}/bbt/coroutine/detail/Processer.cc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pd_source}")
file(READ "${_pd_source}" _pd_original)
set(_pd_probe "${_pd_original}")
set(_pd_join "    if (!leader) {")
set(_pd_continue "        if (any_dequeued && GetExecutableNum() > 0)\n            continue;")
foreach(_point _pd_join _pd_continue)
    string(REPLACE "${${_point}}" "" _removed "${_pd_original}")
    string(LENGTH "${_pd_original}" _old_len)
    string(LENGTH "${_removed}" _new_len)
    string(LENGTH "${${_point}}" _point_len)
    math(EXPR _diff_len "${_old_len} - ${_new_len}")
    if(NOT _diff_len EQUAL _point_len)
        message(FATAL_ERROR "Processer test probe anchor is not unique: ${_point}")
    endif()
endforeach()
string(REPLACE "${_pd_join}" "${_pd_join}\n        ::PDTestJoinerProbe();" _pd_probe "${_pd_probe}")
string(REPLACE "${_pd_continue}"
    "        if (any_dequeued && GetExecutableNum() > 0) {\n            ::PDTestContinueProbe();\n            continue;\n        }" _pd_probe "${_pd_probe}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/Processer_driver_probe.cc"
    "void PDTestJoinerProbe();\nvoid PDTestContinueProbe();\n${_pd_probe}")
add_executable(Test_processer_driver_probe coroutine/Test_processer_driver.cc
    "${CMAKE_CURRENT_BINARY_DIR}/Processer_driver_probe.cc")
target_compile_definitions(Test_processer_driver_probe PRIVATE BBT_PD_CONTROLLED_PROBE)
target_link_libraries(Test_processer_driver_probe ${MY_LIBS})
target_link_options(Test_processer_driver_probe PRIVATE -Wl,--export-dynamic)
foreach(mode
    api_boundaries tick_sequence wait_wakeup unregister_waits
    self_detach
    fail_tick_ret fail_tick_throw fail_wait_ret fail_wait_throw
    reentrancy steal_migrate no_driver_directed query_destruct_release)
    if(mode STREQUAL "self_detach" OR mode STREQUAL "tick_sequence")
        add_test(NAME coroutine.processer_driver_${mode} COMMAND Test_processer_driver_probe)
    else()
        add_test(NAME coroutine.processer_driver_${mode} COMMAND Test_processer_driver)
    endif()
    set_tests_properties(coroutine.processer_driver_${mode}
        PROPERTIES ENVIRONMENT "BBT_PD_MODE=${mode}" TIMEOUT 120 LABELS "unit")
endforeach()

################################################################
# 栈预算诊断（BBT_COROUTINE_STACK_DIAGNOSTICS，见根 CMakeLists option）
# OFF 构建不注册本目标；诊断 TU 的 -fno-instrument-functions 由根 CMake 管理。
################################################################
if (BBT_COROUTINE_STACK_DIAGNOSTICS)
    add_executable(Test_stack_budget_runtime
        coroutine/Test_stack_budget_runtime.cc
        coroutine/stack_budget_uninstrumented.cc)
    target_link_libraries(Test_stack_budget_runtime ${MY_LIBS})
    # 插桩只加在本测试 target（PRIVATE，不传播给用户 target 口径）。
    target_compile_options(Test_stack_budget_runtime PRIVATE -finstrument-functions)
    # 未插桩 helper TU：模拟第三方/历史对象，验证 PARTIAL 覆盖边界。
    set_source_files_properties(coroutine/stack_budget_uninstrumented.cc
        PROPERTIES COMPILE_OPTIONS "-fno-instrument-functions")
    add_test(NAME coroutine.stack_budget_runtime COMMAND Test_stack_budget_runtime)
    set_tests_properties(coroutine.stack_budget_runtime PROPERTIES TIMEOUT 60 LABELS "integration")
endif()

# 静态模型使用真实 GCC 编译产物；所有临时输入/输出只在已忽略构建目录。
if (CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
        AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
        AND CMAKE_CXX_COMPILER_VERSION MATCHES "^15\\.2\\.")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME coroutine.stack_budget_static
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/test_stack_budget.py")
    set_tests_properties(coroutine.stack_budget_static PROPERTIES
        TIMEOUT 180
        LABELS "integration"
        ENVIRONMENT "STACK_BUDGET_TEST_ROOT=${PROJECT_BINARY_DIR}/stack_budget_python;STACK_BUDGET_TEST_COMPILER=${CMAKE_CXX_COMPILER}")
else()
    message(STATUS "coroutine.stack_budget_static: UNSUPPORTED compiler/platform (runtime tests unchanged)")
endif()
