set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 ${Wall_Flag} -O3")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin/benchmark_test)

set(MY_LIBS
    bbt_coroutine
)

add_executable(fatigue_coroutine fatigue_coroutine.cc)
target_link_libraries(fatigue_coroutine ${MY_LIBS})

add_executable(benchmark_coroutine benchmark_coroutine.cc)
target_link_libraries(benchmark_coroutine ${MY_LIBS})

add_executable(benchmark_copool benchmark_copool.cc)
target_link_libraries(benchmark_copool ${MY_LIBS})

add_executable(fatigue_chan fatigue_chan.cc)
target_link_libraries(fatigue_chan ${MY_LIBS})

add_executable(fatigue_comutex fatigue_comutex.cc)
target_link_libraries(fatigue_comutex ${MY_LIBS})

add_executable(fatigue_copool fatigue_copool.cc)
target_link_libraries(fatigue_copool ${MY_LIBS})

add_executable(mem_check_test mem_check_test.cc)
target_link_libraries(mem_check_test ${MY_LIBS})
