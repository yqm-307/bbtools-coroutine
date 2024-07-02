set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 ${Wall_Flag} -g -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 ${Wall_Flag} -g")

enable_testing()
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin/unit_test)
add_definitions(-DBBT_COROUTINE_PROFILE)

set(MY_LIBS
    libboost_unit_test_framework.so
    libboost_test_exec_monitor.so
    bbt_coroutine
    # tcmalloc
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

add_executable(Test_cond Test_cond.cc)
target_link_libraries(Test_cond ${MY_LIBS})
add_test(NAME Test_cond COMMAND Test_cond)

add_executable(Test_g_co Test_g_co.cc)
target_link_libraries(Test_g_co ${MY_LIBS})
add_test(NAME Test_g_co COMMAND Test_g_co)

add_executable(Test_chan Test_chan.cc)
target_link_libraries(Test_chan ${MY_LIBS})
add_test(NAME Test_chan COMMAND Test_chan)

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

add_executable(Debug_regist_co Debug_regist_co.cc)
target_link_libraries(Debug_regist_co ${MY_LIBS})

add_executable(Debug_chan Debug_chan.cc)
target_link_libraries(Debug_chan ${MY_LIBS})

add_executable(Debug_create_coroutine Debug_create_coroutine.cc)
target_link_libraries(Debug_create_coroutine ${MY_LIBS})