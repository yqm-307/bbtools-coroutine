# 固定并发与栈规格路由

[main.cc](main.cc) 展示固定大小的队列消费者，以及把一个任务提交给指定栈规格的 worker。两种用法都先收结果，再由池外调用 `Release()`；`Release()` 不会做完队列里的剩余任务。

运行 `./build/examples/bin/examples/copool`，依次输出 `sum of squares: 14` 和 `routed stack: 131072 bytes`。
