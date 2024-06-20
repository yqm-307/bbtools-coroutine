set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 ${Wall_Flag} -g")

enable_testing()
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

add_executable(Test_hook Test_hook.cc)
target_link_libraries(Test_hook ${MY_LIBS})
add_test(NAME Test_hook COMMAND Test_hook)

add_executable(Test_poller Test_poller.cc)
target_link_libraries(Test_poller ${MY_LIBS})
add_test(NAME Test_poller COMMAND Test_poller)

add_executable(Debug_scheduler Debug_scheduler.cc)
target_link_libraries(Debug_scheduler ${MY_LIBS})

add_executable(Debug_g_co Debug_g_co.cc)
target_link_libraries(Debug_g_co ${MY_LIBS})

add_executable(Debug_poller Debug_poller.cc)
target_link_libraries(Debug_poller ${MY_LIBS})

add_executable(Debug_hook_sleep Debug_hook_sleep.cc)
target_link_libraries(Debug_hook_sleep ${MY_LIBS})

add_executable(Debug_cond Debug_cond.cc)
target_link_libraries(Debug_cond ${MY_LIBS})