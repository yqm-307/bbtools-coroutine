set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti ${Wall_Flag}")

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

add_executable(waitfor waitfor.cc)
target_link_libraries(waitfor ${MY_LIBS})

add_executable(consumer_productor consumer_productor.cc)
target_link_libraries(consumer_productor ${MY_LIBS})
