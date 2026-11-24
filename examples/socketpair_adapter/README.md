# 把 fd 和回调接进协程

[main.cc](main.cc) 用本地 socketpair 演示带截止时间的 fd 等待，再用 `CoWaiter` 模拟提交时同步完成的第三方回调。两种情况都先登记再投递；回调不阻塞，等待方检查状态后再取数据。

运行 `./build/examples/bin/examples/socketpair_adapter`，依次输出 `fd readable: x` 和 `callback result: 42`。无需外部服务或额外线程。
