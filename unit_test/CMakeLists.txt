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

# #232 hook 行为契约基座：不进 smoke（含 sleep(1) 级真实时序，保留给完整回归）
add_executable(Test_hook_contract Test_hook_contract.cc)
target_link_libraries(Test_hook_contract ${MY_LIBS})
add_test(NAME Test_hook_contract COMMAND Test_hook_contract)
set_tests_properties(Test_hook_contract PROPERTIES TIMEOUT 30)

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

add_executable(Test_eventloop_backend Test_eventloop_backend.cc)
target_link_libraries(Test_eventloop_backend ${MY_LIBS})
add_test(NAME Test_eventloop_backend COMMAND Test_eventloop_backend)
set_tests_properties(Test_eventloop_backend PROPERTIES TIMEOUT 10)

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
