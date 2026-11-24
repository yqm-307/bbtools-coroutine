# 从多个缓冲 Chan 等结果

[main.cc](main.cc) 从两个通道选一个，调用方持有两个通道，选择期间不允许关闭。真实业务如果允许关闭，不能把下标直接当成收到了数据，见 [CoSelect API](../../docs/guides/api-reference.md#5-coselect)。

运行 `./build/examples/bin/examples/select`，输出 `case 1: 42`。
