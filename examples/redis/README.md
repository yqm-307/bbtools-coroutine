# 在协程里调用 hiredis

[main.cc](main.cc) 每个池任务独占一个 hiredis context，协程收齐结果后再释放池。示例只针对该客户端的可 Hook socket 路径，不保证其他客户端都可在协程里调用。

安装 hiredis 开发包后构建。**只连接专用的本地测试 Redis**：默认端口 `6379`，可用 `BBT_EXAMPLE_REDIS_PORT` 指向隔离实例。运行 `./build/examples/bin/examples/co_with_hiredis`，输出 `Total successful operations: 10000` 和 `Total errors: 0`。它会写带短期 TTL 的测试 key。
