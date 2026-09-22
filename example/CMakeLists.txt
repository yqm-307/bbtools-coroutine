set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0 ${Wall_Flag}")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin/example)

set(MY_LIBS
    bbt_coroutine
)

add_executable(coroutine coroutine.cc)
target_link_libraries(coroutine ${MY_LIBS})

add_executable(chan chan.cc)
target_link_libraries(chan ${MY_LIBS})

add_executable(cocond cocond.cc)
target_link_libraries(cocond ${MY_LIBS})

add_executable(comutex comutex.cc)
target_link_libraries(comutex ${MY_LIBS})

add_executable(echo_client echoserver/EchoClient.cc)
target_link_libraries(echo_client ${MY_LIBS})

add_executable(echo_server echoserver/EchoServer.cc)
target_link_libraries(echo_server ${MY_LIBS})

add_executable(copool copool.cc)
target_link_libraries(copool ${MY_LIBS})

add_executable(socketpair_adapter socketpair_adapter.cc)
target_link_libraries(socketpair_adapter ${MY_LIBS})

add_executable(waitfor waitfor.cc)
target_link_libraries(waitfor ${MY_LIBS})

add_executable(consumer_productor consumer_productor.cc)
target_link_libraries(consumer_productor ${MY_LIBS})

# hiredis / mysqlclient 为可选外部依赖：缺失时跳过对应示例，
# 不阻塞核心库与其余示例构建。
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)
if (HIREDIS_INCLUDE_DIR AND HIREDIS_LIBRARY)
    add_executable(co_with_hiredis redis/co_with_hiredis.cc)
    target_include_directories(co_with_hiredis PRIVATE ${HIREDIS_INCLUDE_DIR})
    # 注意：bbt_coroutine 需要在 hiredis 之前，这样 hook 函数能够正确覆盖系统调用
    target_link_libraries(co_with_hiredis ${MY_LIBS} ${HIREDIS_LIBRARY} dl)
else()
    message(STATUS "hiredis 未找到，跳过 co_with_hiredis 示例")
endif()

find_path(MYSQL_INCLUDE_DIR mysql/mysql.h)
find_library(MYSQL_LIBRARY mysqlclient)
if (MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
    add_executable(co_with_mysql mysqlpp/co_with_mysql.cc)
    target_include_directories(co_with_mysql PRIVATE ${MYSQL_INCLUDE_DIR})
    target_link_libraries(co_with_mysql ${MY_LIBS} pthread ${MYSQL_LIBRARY} dl)
else()
    message(STATUS "mysqlclient 未找到，跳过 co_with_mysql 示例")
endif()

if (NEED_TEST)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # Echo 段是必跑验收：无条件注册、必须 PASS（不设 SKIP_RETURN_CODE），
    # 不再因 hiredis 缺失把已通过的 echo 证据记为 SKIP。
    add_test(
        NAME Test_real_clients_echo
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/acceptance_real_clients.py
                --mode echo --echo-server $<TARGET_FILE:echo_server>)
    set_tests_properties(Test_real_clients_echo PROPERTIES
        TIMEOUT 180
        RUN_SERIAL TRUE)
    # hiredis 段仅在 target 存在（系统装有 hiredis 库）时注册；
    # 运行时 Redis 不可达仍以 SKIP=77 跳过，不伪造通过。
    if (TARGET co_with_hiredis)
        add_test(
            NAME Test_real_clients_hiredis
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/scripts/acceptance_real_clients.py
                    --mode hiredis --hiredis $<TARGET_FILE:co_with_hiredis>)
        set_tests_properties(Test_real_clients_hiredis PROPERTIES
            TIMEOUT 180
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77)
    endif()
endif()