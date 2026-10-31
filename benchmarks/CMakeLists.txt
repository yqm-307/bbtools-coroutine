set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 ${Wall_Flag} -O3")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin/benchmarks)

set(MY_LIBS
    bbt_coroutine
)

add_executable(fatigue_coroutine fatigue_coroutine.cc)
target_link_libraries(fatigue_coroutine ${MY_LIBS})

# 仅构建；短跑/长跑均由调用方显式启动，不注册自动 benchmark。
add_executable(benchmark_processer_scaling benchmark_processer_scaling.cc)
target_link_libraries(benchmark_processer_scaling ${MY_LIBS})
option(BBT_SCALING_LEGACY_RUNTIME "仅在已核实的旧 runtime 上构建 fixed-old 基准" OFF)
if(BBT_SCALING_LEGACY_RUNTIME)
    target_compile_definitions(benchmark_processer_scaling PRIVATE BBT_SCALING_LEGACY_RUNTIME)
endif()

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

add_executable(fatigue_cocond fatigue_cocond.cc)
target_link_libraries(fatigue_cocond ${MY_LIBS})

add_executable(fatigue_std_uniquelock fatigue_std_uniquelock.cc)
target_link_libraries(fatigue_std_uniquelock ${MY_LIBS})

add_executable(mem_check_test mem_check_test.cc)
target_link_libraries(mem_check_test ${MY_LIBS})

add_executable(benchmark_cocond benchmark_cocond.cc)
target_link_libraries(benchmark_cocond ${MY_LIBS})

add_executable(benchmark_chan benchmark_chan.cc)
target_link_libraries(benchmark_chan ${MY_LIBS})

add_executable(benchmark_comutex benchmark_comutex.cc)
target_link_libraries(benchmark_comutex ${MY_LIBS})

add_executable(fatigue_corwmutex fatigue_corwmutex.cc)
target_link_libraries(fatigue_corwmutex ${MY_LIBS})

add_executable(unified_stress unified_stress.cc)
target_link_libraries(unified_stress ${MY_LIBS} pthread)

add_executable(cocond_queue_stress cocond_queue_stress.cc)
target_link_libraries(cocond_queue_stress ${MY_LIBS} pthread)