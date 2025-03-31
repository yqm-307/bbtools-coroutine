if [ ! -d "bin" ]; then
    mkdir bin
fi

g++ -o bin/libgo_test 100w_task_libgo.cc -llibgo -ldl -lbbt_core
g++ -o bin/bbtco_test 100w_task_bbtco.cc -lbbt_core -lbbt_coroutine -lpthread -levent_pthreads

sleep 1
if [ -f "./bin/libgo_test" ]; then
    echo "libgo_test begin"
    time ./bin/libgo_test
fi


sleep 1
if [ -f "./bin/bbtco_test" ]; then
    echo "bbtco_test begin"
    time ./bin/bbtco_test
fi


sleep 1
echo "go test begin"
time go run "100w_task_go.go"