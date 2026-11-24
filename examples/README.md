# 可运行示例

从 [启动一个协程并等它完成](coroutine/README.md) 开始。写新代码时先找与你的任务最接近的一行，点进去看完整 `.cc`；需要返回码、挂起条件和寿命规则，再查 [使用指南](../docs/guides/user-guide.md) 和 [API 参考](../docs/guides/api-reference.md)。

在仓库根目录构建和运行：

```bash
cmake -S . -B build/examples -G Ninja -DNEED_EXAMPLE=ON -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
flock /tmp/bbt-build.lock cmake --build build/examples --parallel 2
./build/examples/bin/examples/coroutine
```

只构建一项时，加 `--target <目标>`。这些例子演示用法，行为断言和回归在 `tests/`，不要把测试里的调度器启动方式复制进应用。

| 想做什么 | 示例 / 目标 | 重点 |
| --- | --- | --- |
| 启动协程并等结果 | [coroutine](coroutine/README.md) | `BBTCO_MAIN`、命名、让出、睡眠、defer、完成通知 |
| 把异步步骤写成顺序代码 | [async](async/README.md) | 早返回、只移动结果、顺序等待、惰性绑定和 detached 提交 |
| 在协程之间交接数据 | [chan](chan/README.md) | 缓冲与无缓冲、关闭后收完数据 |
| 从多个通道等一条消息 | [select](select/README.md) | `CoSelect`、超时、通道保活与关闭约束 |
| 保护共享状态 | [comutex](comutex/README.md) | RAII 互斥锁、读锁和写锁 |
| 唤醒已登记的等待者 | [cocond](cocond/README.md) | 单 worker 通知示范；通用完成通知优先用 Chan |
| 固定任务并发数 | [copool](copool/README.md) | 等结果再释放、按栈规格路由 |
| 延时或登记一次性回调 | [waitfor](waitfor/README.md) | 当前协程等待和新协程回调的区别 |
| 接 fd 或外部回调 | [socketpair_adapter](socketpair_adapter/README.md) | 区分就绪与完成；登记后投递、带载荷通知 |
| 用 Hook 写本机 TCP | [echo_server / echo_client](echoserver/README.md) | 每连接一个协程、短读短写、fd 所有权 |
| 接第三方客户端 | [co_with_hiredis](redis/README.md)、[co_with_mysql](mysqlpp/README.md) | 独占 Redis context；把阻塞式 MySQL 探针留在原生线程 |

Redis / MySQL 目标只在找到相应客户端开发包时构建，运行需要自己的本地测试服务。`stack_budget/` 是 [独立的两阶段栈预算工具](../docs/guides/stack-budget.md)，不是应用示例。
