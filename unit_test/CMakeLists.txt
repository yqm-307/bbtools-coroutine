set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 ${Wall_Flag} -g -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti ${Wall_Flag}")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin/unit_test)

set(MY_LIBS
    # 测试文件通过 BOOST_TEST_MAIN + boost/test/included/unit_test.hpp 静态内嵌 Boost.Test，
    # 不得再链接 libboost_unit_test_framework.so：共享库与二进制各持一份框架全局状态，
    # ELF 符号插入使 .so 析构释放二进制已释放的对象，__cxa_finalize 阶段 double-free。
    bbt_coroutine
)

add_executable(Test_coroutine_stack Test_coroutine_stack.cc)
target_link_libraries(Test_coroutine_stack ${MY_LIBS})
add_test(NAME Test_coroutine_stack COMMAND Test_coroutine_stack)

add_executable(Test_coroutine Test_coroutine.cc)
target_link_libraries(Test_coroutine ${MY_LIBS})
add_test(NAME Test_coroutine COMMAND Test_coroutine)

add_executable(Test_coroutine_api Test_coroutine_api.cc)
target_link_libraries(Test_coroutine_api ${MY_LIBS})
add_test(NAME Test_coroutine_api COMMAND Test_coroutine_api)

add_executable(Test_coroutine_exception Test_coroutine_exception.cc)
target_link_libraries(Test_coroutine_exception ${MY_LIBS})
add_test(NAME Test_coroutine_exception COMMAND Test_coroutine_exception)

add_executable(Test_scheduler_exec Test_scheduler_exec.cc)
target_link_libraries(Test_scheduler_exec ${MY_LIBS})
add_test(NAME Test_scheduler_exec COMMAND Test_scheduler_exec)
set_tests_properties(Test_scheduler_exec PROPERTIES TIMEOUT 15)

# 跨 worker 恢复与用户 TLS 边界回归（#339）：含事件唤醒迁移，耐心等待但必须有界。
add_executable(Test_worker_tls_boundary Test_worker_tls_boundary.cc)
target_link_libraries(Test_worker_tls_boundary ${MY_LIBS})
add_test(NAME Test_worker_tls_boundary COMMAND Test_worker_tls_boundary)
set_tests_properties(Test_worker_tls_boundary PROPERTIES TIMEOUT 40)

add_executable(Test_coroutine_cancel Test_coroutine_cancel.cc)
target_link_libraries(Test_coroutine_cancel ${MY_LIBS})
add_test(NAME Test_coroutine_cancel COMMAND Test_coroutine_cancel)
set_tests_properties(Test_coroutine_cancel PROPERTIES TIMEOUT 10)

add_executable(Test_exception_stop Test_exception_stop.cc)
target_link_libraries(Test_exception_stop ${MY_LIBS})
add_test(NAME Test_exception_stop COMMAND Test_exception_stop)
set_tests_properties(Test_exception_stop PROPERTIES TIMEOUT 10)

add_executable(Test_hook Test_hook.cc)
target_link_libraries(Test_hook ${MY_LIBS})
add_test(NAME Test_hook COMMAND Test_hook)

add_executable(Test_dns_lifecycle Test_dns_lifecycle.cc)
target_link_libraries(Test_dns_lifecycle ${MY_LIBS})
add_test(NAME Test_dns_lifecycle COMMAND Test_dns_lifecycle)
set_tests_properties(Test_dns_lifecycle PROPERTIES TIMEOUT 10)

# #232 hook 行为契约基座：不进 smoke（含 sleep(1) 级真实时序，保留给完整回归）
add_executable(Test_hook_contract Test_hook_contract.cc)
target_link_libraries(Test_hook_contract ${MY_LIBS})
add_test(NAME Test_hook_contract COMMAND Test_hook_contract)
set_tests_properties(Test_hook_contract PROPERTIES TIMEOUT 60)

add_executable(Test_hook_blocking_fd Test_hook_blocking_fd.cc)
target_link_libraries(Test_hook_blocking_fd ${MY_LIBS})
add_test(NAME Test_hook_blocking_fd COMMAND Test_hook_blocking_fd)
set_tests_properties(Test_hook_blocking_fd PROPERTIES TIMEOUT 10)

add_executable(Test_coroutine_diag Test_coroutine_diag.cc)
target_link_libraries(Test_coroutine_diag ${MY_LIBS})
add_test(NAME Test_coroutine_diag COMMAND Test_coroutine_diag)
set_tests_properties(Test_coroutine_diag PROPERTIES TIMEOUT 15)

add_executable(Test_hook_timeout_flags Test_hook_timeout_flags.cc)
target_link_libraries(Test_hook_timeout_flags ${MY_LIBS})
add_test(NAME Test_hook_timeout_flags COMMAND Test_hook_timeout_flags)
set_tests_properties(Test_hook_timeout_flags PROPERTIES TIMEOUT 15)
add_executable(Test_worker_stall Test_worker_stall.cc)
target_link_libraries(Test_worker_stall ${MY_LIBS})
add_test(NAME Test_worker_stall COMMAND Test_worker_stall)
set_tests_properties(Test_worker_stall PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_executable(Test_crash_diag Test_crash_diag.cc)
target_link_libraries(Test_crash_diag ${MY_LIBS})
add_test(NAME Test_crash_diag COMMAND Test_crash_diag)
set_tests_properties(Test_crash_diag PROPERTIES TIMEOUT 30)

add_executable(Test_hook_error_matrix Test_hook_error_matrix.cc)
target_link_libraries(Test_hook_error_matrix ${MY_LIBS})
add_test(NAME Test_hook_error_matrix COMMAND Test_hook_error_matrix)
set_tests_properties(Test_hook_error_matrix PROPERTIES TIMEOUT 20)

# #370 FD epoch 线性化回归：close+reuse 的三类判别性用例
add_executable(Test_hook_fd_epoch Test_hook_fd_epoch.cc)
target_link_libraries(Test_hook_fd_epoch ${MY_LIBS})
add_test(NAME Test_hook_fd_epoch COMMAND Test_hook_fd_epoch)
set_tests_properties(Test_hook_fd_epoch PROPERTIES TIMEOUT 20)

# 多 worker close+reuse 必须独立进程：worker 数在首次 Start 时固定，
# 同一可执行内无法从 1 切到 4（一次初始化契约），故单列一个进程。
add_executable(Test_hook_fd_epoch_multi Test_hook_fd_epoch_multi.cc)
target_link_libraries(Test_hook_fd_epoch_multi ${MY_LIBS})
add_test(NAME Test_hook_fd_epoch_multi COMMAND Test_hook_fd_epoch_multi)
set_tests_properties(Test_hook_fd_epoch_multi PROPERTIES TIMEOUT 20)

add_executable(Test_poller Test_poller.cc)
target_link_libraries(Test_poller ${MY_LIBS})
add_test(NAME Test_poller COMMAND Test_poller)

add_executable(Test_eventloop_boundary Test_eventloop_boundary.cc)
target_link_libraries(Test_eventloop_boundary ${MY_LIBS})
add_test(NAME Test_eventloop_boundary COMMAND Test_eventloop_boundary)
set_tests_properties(Test_eventloop_boundary PROPERTIES TIMEOUT 10)

add_executable(Test_copollevent_state Test_copollevent_state.cc)
target_link_libraries(Test_copollevent_state ${MY_LIBS})
add_test(NAME Test_copollevent_state COMMAND Test_copollevent_state)
set_tests_properties(Test_copollevent_state PROPERTIES TIMEOUT 30)

add_executable(Test_copoller_deferred_owner Test_copoller_deferred_owner.cc)
target_link_libraries(Test_copoller_deferred_owner ${MY_LIBS})
add_test(NAME Test_copoller_deferred_owner COMMAND Test_copoller_deferred_owner)
set_tests_properties(Test_copoller_deferred_owner PROPERTIES TIMEOUT 15)

add_executable(Test_eventloop_backend Test_eventloop_backend.cc)
target_link_libraries(Test_eventloop_backend ${MY_LIBS})
add_test(NAME Test_eventloop_backend COMMAND Test_eventloop_backend)
set_tests_properties(Test_eventloop_backend PROPERTIES TIMEOUT 10)

add_executable(Test_eventloop_contract Test_eventloop_contract.cc)
target_link_libraries(Test_eventloop_contract ${MY_LIBS})
add_test(NAME Test_eventloop_contract COMMAND Test_eventloop_contract)
set_tests_properties(Test_eventloop_contract PROPERTIES TIMEOUT 10)

add_executable(Test_io_executor Test_io_executor.cc)
target_link_libraries(Test_io_executor ${MY_LIBS})
add_test(NAME Test_io_executor COMMAND Test_io_executor)
set_tests_properties(Test_io_executor PROPERTIES TIMEOUT 15)

# 迁入自 bbtools-core 的 pollevent / thread 直接单测（Issue #12 P2），
# 覆盖 EventBase/EventLoop/Event/EvThread 与 CountDownLatch 的独立行为。
add_executable(Test_eventloop Test_eventloop.cc)
target_link_libraries(Test_eventloop ${MY_LIBS})
add_test(NAME Test_eventloop COMMAND Test_eventloop)
set_tests_properties(Test_eventloop PROPERTIES TIMEOUT 30)

add_executable(Test_countdown_latch Test_countdown_latch.cc)
target_link_libraries(Test_countdown_latch ${MY_LIBS})
add_test(NAME Test_countdown_latch COMMAND Test_countdown_latch)
set_tests_properties(Test_countdown_latch PROPERTIES TIMEOUT 15)

add_executable(Test_cond Test_cond.cc)
target_link_libraries(Test_cond ${MY_LIBS})
add_test(NAME Test_cond COMMAND Test_cond)

add_executable(Test_g_co Test_g_co.cc)
target_link_libraries(Test_g_co ${MY_LIBS})
add_test(NAME Test_g_co COMMAND Test_g_co)

add_executable(Test_macro_wrap Test_macro_wrap.cc)
target_link_libraries(Test_macro_wrap ${MY_LIBS})
add_test(NAME Test_macro_wrap COMMAND Test_macro_wrap)
set_tests_properties(Test_macro_wrap PROPERTIES TIMEOUT 10)

add_executable(Test_scheduler_api Test_scheduler_api.cc)
target_link_libraries(Test_scheduler_api ${MY_LIBS})
add_test(NAME Test_scheduler_api COMMAND Test_scheduler_api)
set_tests_properties(Test_scheduler_api PROPERTIES TIMEOUT 10)

add_executable(Test_chan Test_chan.cc)
target_link_libraries(Test_chan ${MY_LIBS})
add_test(NAME Test_chan COMMAND Test_chan)

add_executable(Test_coselect Test_coselect.cc)
target_link_libraries(Test_coselect ${MY_LIBS})
add_test(NAME Test_coselect COMMAND Test_coselect)
set_tests_properties(Test_coselect PROPERTIES TIMEOUT 30)

add_executable(Test_comutex Test_comutex.cc)
target_link_libraries(Test_comutex ${MY_LIBS})
add_test(NAME Test_comutex COMMAND Test_comutex)

add_executable(Test_lockguard Test_lockguard.cc)
target_link_libraries(Test_lockguard ${MY_LIBS})
add_test(NAME Test_lockguard COMMAND Test_lockguard)

add_executable(Test_rwlock_guard Test_rwlock_guard.cc)
target_link_libraries(Test_rwlock_guard ${MY_LIBS})
add_test(NAME Test_rwlock_guard COMMAND Test_rwlock_guard)

add_executable(Test_defer Test_defer.cc)
target_link_libraries(Test_defer ${MY_LIBS})
add_test(NAME Test_defer COMMAND Test_defer)

add_executable(Test_profiler Test_profiler.cc)
target_link_libraries(Test_profiler ${MY_LIBS})
add_test(NAME Test_profiler COMMAND Test_profiler)

add_executable(Test_co_rwmutex Test_co_rwmutex.cc)
target_link_libraries(Test_co_rwmutex ${MY_LIBS})
add_test(NAME Test_co_rwmutex COMMAND Test_co_rwmutex)
set_tests_properties(Test_co_rwmutex PROPERTIES TIMEOUT 30)

add_executable(Test_coevent Test_coevent.cc)
target_link_libraries(Test_coevent ${MY_LIBS})
add_test(NAME Test_coevent COMMAND Test_coevent)

add_executable(Test_copool Test_copool.cc)
target_link_libraries(Test_copool ${MY_LIBS})
add_test(NAME Test_copool COMMAND Test_copool)

add_executable(Test_smoke Test_smoke.cc)
target_link_libraries(Test_smoke ${MY_LIBS})
add_test(NAME Test_smoke COMMAND Test_smoke)

add_executable(Test_reliability Test_reliability.cc)
target_link_libraries(Test_reliability ${MY_LIBS})
add_test(NAME Test_reliability COMMAND Test_reliability)

# #347 C0：对象身份与运行时代际（C-01）
add_executable(Test_co_object_identity Test_co_object_identity.cc)
target_link_libraries(Test_co_object_identity ${MY_LIBS})
add_test(NAME Test_co_object_identity COMMAND Test_co_object_identity)
set_tests_properties(Test_co_object_identity PROPERTIES TIMEOUT 15)

# #347 切片②：唤醒原因独立化 + CoWaiter 窄接口单次等待结果收口
add_executable(Test_cowaiter_outcome Test_cowaiter_outcome.cc)
target_link_libraries(Test_cowaiter_outcome ${MY_LIBS})
add_test(NAME Test_cowaiter_outcome COMMAND Test_cowaiter_outcome)
set_tests_properties(Test_cowaiter_outcome PROPERTIES TIMEOUT 30)

# #346：CoWaiter 组合等待最小稳定入口（单 FD interest + deadline + Notify + 取消首胜）
add_executable(Test_combined_wait Test_combined_wait.cc)
target_link_libraries(Test_combined_wait ${MY_LIBS})
add_test(NAME Test_combined_wait COMMAND Test_combined_wait)
set_tests_properties(Test_combined_wait PROPERTIES TIMEOUT 30)

# #369：旧超时族接口登记失败回归（固定失败注入 _TestFailNextAwaitRegist）
add_executable(Test_cowaiter_regist_fail Test_cowaiter_regist_fail.cc)
target_link_libraries(Test_cowaiter_regist_fail ${MY_LIBS})
add_test(NAME Test_cowaiter_regist_fail COMMAND Test_cowaiter_regist_fail)
set_tests_properties(Test_cowaiter_regist_fail PROPERTIES TIMEOUT 30)

# Smoke 候选：CI 快速回归集，统一 60s 超时防止无进展挂死。
# Test_reliability 不进 Smoke，保留给完整回归与独立 Reliability Job。
set_tests_properties(
    Test_coroutine
    Test_coroutine_exception
    Test_poller
    Test_chan
    Test_comutex
    Test_coevent
    Test_copool
    Test_smoke
    PROPERTIES LABELS "smoke" TIMEOUT 60
)

################################################################
# coroutine process-lifetime 公共迁移（coroutine 隔离实现）
#  - CoEventValue / 带参等待 新公共面
#  - 公共头逐头独立 TU 编译（仅含伞头不能证明每个头自给）
#  - CoEventValue 跨 DSO 类型识别（两个独立产物 + 一个消费方）
################################################################

add_executable(Test_coeventvalue Test_coeventvalue.cc)
target_link_libraries(Test_coeventvalue ${MY_LIBS})
add_test(NAME Test_coeventvalue COMMAND Test_coeventvalue)
set_tests_properties(Test_coeventvalue PROPERTIES TIMEOUT 15)

add_executable(Test_cowaiter_payload Test_cowaiter_payload.cc)
target_link_libraries(Test_cowaiter_payload ${MY_LIBS})
add_test(NAME Test_cowaiter_payload COMMAND Test_cowaiter_payload)
set_tests_properties(Test_cowaiter_payload PROPERTIES TIMEOUT 30)

# 等待代次载荷隔离：确定性交错需单 worker，单列进程固定 1 worker。
add_executable(Test_cowaiter_payload_rounds Test_cowaiter_payload_rounds.cc)
target_link_libraries(Test_cowaiter_payload_rounds ${MY_LIBS})
add_test(NAME Test_cowaiter_payload_rounds COMMAND Test_cowaiter_payload_rounds)
set_tests_properties(Test_cowaiter_payload_rounds PROPERTIES TIMEOUT 30)

# 三种 Start 模式 / 并发 Start / 进程 exit：各自独立子进程（fork）验证
add_executable(Test_scheduler_start_modes Test_scheduler_start_modes.cc)
target_link_libraries(Test_scheduler_start_modes ${MY_LIBS})
add_test(NAME Test_scheduler_start_modes COMMAND Test_scheduler_start_modes)
set_tests_properties(Test_scheduler_start_modes PROPERTIES TIMEOUT 30)

# 驱动模式不能在同一可执行内切换：NO_LOOP + LoopOnce 单列进程
add_executable(Test_scheduler_noloop Test_scheduler_noloop.cc)
target_link_libraries(Test_scheduler_noloop ${MY_LIBS})
add_test(NAME Test_scheduler_noloop COMMAND Test_scheduler_noloop)
set_tests_properties(Test_scheduler_noloop PROPERTIES TIMEOUT 20)

# 跨 DSO：a 构造载荷、b 读取；两侧 traits 依赖公共头的显式登记 ID
add_library(coeventvalue_dso_a SHARED coeventvalue_dso_a.cc)
target_link_libraries(coeventvalue_dso_a ${MY_LIBS})
add_library(coeventvalue_dso_b SHARED coeventvalue_dso_b.cc)
target_link_libraries(coeventvalue_dso_b ${MY_LIBS})
add_executable(Test_coeventvalue_dso Test_coeventvalue_dso.cc)
target_link_libraries(Test_coeventvalue_dso ${MY_LIBS} coeventvalue_dso_a coeventvalue_dso_b)
add_test(NAME Test_coeventvalue_dso COMMAND Test_coeventvalue_dso)
set_tests_properties(Test_coeventvalue_dso PROPERTIES TIMEOUT 15)

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
