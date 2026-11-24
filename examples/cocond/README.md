# 唤醒已登记的等待者

[main.cc](main.cc) 只启动一个 worker，让主协程先进入等待，再让通知协程运行。不要把这个顺序推广到多 worker；`CoCond` 不会释放业务锁，也不会补发早到的通知。通用任务完成用 [Chan](../chan/README.md)。

运行 `./build/examples/bin/examples/cocond`，输出 `waiter notified`。
