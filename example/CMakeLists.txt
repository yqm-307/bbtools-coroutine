set(Wall_Flag "-Wall -Wno-sign-compare -Wno-format -Wno-reorder -Wno-unknown-pragmas")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++17 ${Wall_Flag} -g -O2")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin/example)

set(MY_LIBS
    bbt_coroutine
)

add_executable(coroutine coroutine.cc)
target_link_libraries(coroutine ${MY_LIBS})

add_executable(chan chan.cc)
target_link_libraries(chan ${MY_LIBS})