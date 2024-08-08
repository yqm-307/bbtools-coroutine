set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 ${Wall_Flag} -g -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 ${Wall_Flag} -g")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin/unit_test)

set(MY_LIBS
    bbt_coroutine_dbg
    # tcmalloc
)

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

add_executable(Debug_steal Debug_steal.cc)
target_link_libraries(Debug_steal ${MY_LIBS})

add_executable(Debug_hook_connect_bug Debug_hook_connect_bug.cc)
target_link_libraries(Debug_hook_connect_bug ${MY_LIBS})