set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 ${Wall_Flag} -g -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti ${Wall_Flag}")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin/unit_test)

set(MY_LIBS
    libboost_unit_test_framework.so
    libboost_test_exec_monitor.so
    bbt_coroutine
)

add_executable(Test_coroutine_stack Test_coroutine_stack.cc)
target_link_libraries(Test_coroutine_stack ${MY_LIBS})
add_test(NAME Test_coroutine_stack COMMAND Test_coroutine_stack)

add_executable(Test_coroutine Test_coroutine.cc)
target_link_libraries(Test_coroutine ${MY_LIBS})
add_test(NAME Test_coroutine COMMAND Test_coroutine)

add_executable(Test_coroutine_exception Test_coroutine_exception.cc)
target_link_libraries(Test_coroutine_exception ${MY_LIBS})
add_test(NAME Test_coroutine_exception COMMAND Test_coroutine_exception)

add_executable(Test_public_api_contract Test_public_api_contract.cc)
target_link_libraries(Test_public_api_contract ${MY_LIBS})
add_test(NAME Test_public_api_contract COMMAND Test_public_api_contract)

add_executable(Test_syntax_macro_contract Test_syntax_macro_contract.cc)
target_link_libraries(Test_syntax_macro_contract ${MY_LIBS})
add_test(NAME Test_syntax_macro_contract COMMAND Test_syntax_macro_contract)

add_executable(Test_scheduler_shutdown Test_scheduler_shutdown.cc)
target_link_libraries(Test_scheduler_shutdown ${MY_LIBS})
add_test(NAME Test_scheduler_shutdown COMMAND Test_scheduler_shutdown)

add_executable(Test_scheduler_concurrency Test_scheduler_concurrency.cc)
target_link_libraries(Test_scheduler_concurrency ${MY_LIBS})
add_test(NAME Test_scheduler_concurrency COMMAND Test_scheduler_concurrency)

add_executable(Test_coroutine_lifecycle_state Test_coroutine_lifecycle_state.cc)
target_link_libraries(Test_coroutine_lifecycle_state ${MY_LIBS})
add_test(NAME Test_coroutine_lifecycle_state COMMAND Test_coroutine_lifecycle_state)

add_executable(Test_coroutine_lifecycle_teardown Test_coroutine_lifecycle_teardown.cc)
target_link_libraries(Test_coroutine_lifecycle_teardown ${MY_LIBS})
add_test(NAME Test_coroutine_lifecycle_teardown COMMAND Test_coroutine_lifecycle_teardown)

add_executable(Test_coroutine_lifecycle_stress Test_coroutine_lifecycle_stress.cc)
target_link_libraries(Test_coroutine_lifecycle_stress ${MY_LIBS})
add_test(NAME Test_coroutine_lifecycle_stress COMMAND Test_coroutine_lifecycle_stress)

add_executable(Test_hook Test_hook.cc)
target_link_libraries(Test_hook ${MY_LIBS})
add_test(NAME Test_hook COMMAND Test_hook)

add_executable(Test_poller Test_poller.cc)
target_link_libraries(Test_poller ${MY_LIBS})
add_test(NAME Test_poller COMMAND Test_poller)

add_executable(Test_wait_protocol_state Test_wait_protocol_state.cc)
target_link_libraries(Test_wait_protocol_state ${MY_LIBS})
add_test(NAME Test_wait_protocol_state COMMAND Test_wait_protocol_state)

add_executable(Test_wait_protocol_race Test_wait_protocol_race.cc)
target_link_libraries(Test_wait_protocol_race ${MY_LIBS})
add_test(NAME Test_wait_protocol_race COMMAND Test_wait_protocol_race)

add_executable(Test_wait_protocol_waiter_lifetime Test_wait_protocol_waiter_lifetime.cc)
target_link_libraries(Test_wait_protocol_waiter_lifetime ${MY_LIBS})
add_test(NAME Test_wait_protocol_waiter_lifetime COMMAND Test_wait_protocol_waiter_lifetime)

add_executable(Test_wait_protocol_stress Test_wait_protocol_stress.cc)
target_link_libraries(Test_wait_protocol_stress ${MY_LIBS})
add_test(NAME Test_wait_protocol_stress COMMAND Test_wait_protocol_stress)

add_executable(Test_cond Test_cond.cc)
target_link_libraries(Test_cond ${MY_LIBS})
add_test(NAME Test_cond COMMAND Test_cond)

add_executable(Test_g_co Test_g_co.cc)
target_link_libraries(Test_g_co ${MY_LIBS})
add_test(NAME Test_g_co COMMAND Test_g_co)

add_executable(Test_chan Test_chan.cc)
target_link_libraries(Test_chan ${MY_LIBS})
add_test(NAME Test_chan COMMAND Test_chan)

add_executable(Test_comutex Test_comutex.cc)
target_link_libraries(Test_comutex ${MY_LIBS})
add_test(NAME Test_comutex COMMAND Test_comutex)

add_executable(Test_defer Test_defer.cc)
target_link_libraries(Test_defer ${MY_LIBS})
add_test(NAME Test_defer COMMAND Test_defer)

add_executable(Test_co_rwmutex Test_co_rwmutex.cc)
target_link_libraries(Test_co_rwmutex ${MY_LIBS})
add_test(NAME Test_co_rwmutex COMMAND Test_co_rwmutex)

add_executable(Test_coevent Test_coevent.cc)
target_link_libraries(Test_coevent ${MY_LIBS})
add_test(NAME Test_coevent COMMAND Test_coevent)

add_executable(Test_copool Test_copool.cc)
target_link_libraries(Test_copool ${MY_LIBS})
add_test(NAME Test_copool COMMAND Test_copool)