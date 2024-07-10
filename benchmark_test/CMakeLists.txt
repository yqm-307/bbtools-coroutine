set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 ${Wall_Flag} -g -O2")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin/benchmark_test)

set(MY_LIBS
    bbt_coroutine
)

add_executable(coroutine_fatigue_test coroutine_fatigue_test.cc)
target_link_libraries(coroutine_fatigue_test ${MY_LIBS})

add_executable(coroutine_bench_test coroutine_bench_test.cc)
target_link_libraries(coroutine_bench_test ${MY_LIBS})

add_executable(chan_fatigue_test chan_fatigue_test.cc)
target_link_libraries(chan_fatigue_test ${MY_LIBS})

add_executable(mem_check_test mem_check_test.cc)
target_link_libraries(mem_check_test ${MY_LIBS})