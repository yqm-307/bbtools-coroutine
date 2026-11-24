# 顺序等待与延迟投递

[main.cc](main.cc) 的 `BuildAnswer` 在调用时执行，两次 `await` 后返回只移动值。`bbt_co_bind` 则保存 callable 和数据，`fire` 消费它后另起协程；本例另用 Chan 等发布完成，不靠 `fire` 的返回值。

运行 `./build/examples/bin/examples/async`，输出 `published: 42`。完整约束见 [Async 调用指南](../../docs/guides/async-usage.md)。
