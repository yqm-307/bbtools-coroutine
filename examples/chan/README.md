# 用 Chan 交接数据

[main.cc](main.cc) 用同一段代码比较有缓冲通道和无缓冲通道。生产者退出时关闭通道；读者用 `Read()` 消费到关闭，不用 `TryRead()` 排空已关闭的缓冲。

运行 `./build/examples/bin/examples/chan`，依次输出 `capacity 2: total 60` 和 `capacity 0: total 60`。
