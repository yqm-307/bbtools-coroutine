# 两种定时器用法

[main.cc](main.cc) 先用 `bbtco_wait_for` 在当前协程中等定时器，再用 `bbtco_ev_t` 登记一次性回调，触发时另起协程。`bbtco_wait_for` 的正常返回不区分 I/O 就绪与超时。

运行 `./build/examples/bin/examples/waitfor`，依次输出 `main coroutine resumed` 和 `timer callback ran`。
