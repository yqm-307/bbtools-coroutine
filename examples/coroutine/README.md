# 启动并等待一个协程

[main.cc](main.cc) 从 `BBTCO_MAIN` 进入受管协程，用 `bbtco_desc` 启动带名字的任务。`yield` 和 `sleep` 不代表任务已经结束；主协程等 Chan 中的结果。

从仓库根目录运行 `./build/examples/bin/examples/coroutine`，输出 `worker result: 42`。
