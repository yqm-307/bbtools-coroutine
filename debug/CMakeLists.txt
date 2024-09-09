set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 ${Wall_Flag} -g")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin/debug)

set(MY_DBG_LIBS
    bbt_coroutine
)

add_executable(Debug_scheduler Debug_scheduler.cc)
target_link_libraries(Debug_scheduler ${MY_DBG_LIBS})

add_executable(Debug_g_co Debug_g_co.cc)
target_link_libraries(Debug_g_co ${MY_DBG_LIBS})

add_executable(Debug_poller Debug_poller.cc)
target_link_libraries(Debug_poller ${MY_DBG_LIBS})

add_executable(Debug_hook_sleep Debug_hook_sleep.cc)
target_link_libraries(Debug_hook_sleep ${MY_DBG_LIBS})

add_executable(Debug_cond Debug_cond.cc)
target_link_libraries(Debug_cond ${MY_DBG_LIBS})

add_executable(Debug_regist_co Debug_regist_co.cc)
target_link_libraries(Debug_regist_co ${MY_DBG_LIBS})

add_executable(Debug_chan Debug_chan.cc)
target_link_libraries(Debug_chan ${MY_DBG_LIBS})

add_executable(Debug_create_coroutine Debug_create_coroutine.cc)
target_link_libraries(Debug_create_coroutine ${MY_DBG_LIBS})

add_executable(Debug_steal Debug_steal.cc)
target_link_libraries(Debug_steal ${MY_DBG_LIBS})

add_executable(Debug_dbgprintf Debug_dbgprintf.cc)
target_link_libraries(Debug_dbgprintf ${MY_DBG_LIBS})

add_executable(Debug_hook_connect Debug_hook_connect.cc)
target_link_libraries(Debug_hook_connect ${MY_DBG_LIBS})

add_executable(Debug_comutex Debug_comutex.cc)
target_link_libraries(Debug_comutex ${MY_DBG_LIBS})

add_executable(Debug_co_rwmutex Debug_co_rwmutex.cc)
target_link_libraries(Debug_co_rwmutex ${MY_DBG_LIBS})

add_executable(Debug_bbtcoevent Debug_bbtcoevent.cc)
target_link_libraries(Debug_bbtcoevent ${MY_DBG_LIBS})

add_executable(Debug_bbtcoevent Debug_bbtcoevent.cc)
target_link_libraries(Debug_bbtcoevent ${MY_DBG_LIBS})