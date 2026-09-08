# M1-07 真实客户端验收场景

## 目的

用真实阻塞式客户端验证协程 Hook 不只在单元测试中成立。测试入口为
`scripts/acceptance_real_clients.py`，随机数据使用固定 seed，失败可以复现；
不把无约束随机 fuzz 当作验收依据。

## Echo 场景

由仓内 `example/echoserver/EchoServer.cc` 提供 TCP 服务，Python 标准库
socket 作为阻塞式客户端，覆盖：

- 多客户端并发连接、重复重连；
- 1、2、7、15、31、32、33、64、127 字节 payload；
- 随机二进制内容，包含 `NUL` 和非 ASCII 字节；
- 1–12 字节碎片发送，模拟 TCP 分段和发送间隙；
- 每连接多轮请求，连接保持和连接间空闲；
- 客户端半关闭写端，验证服务端回收；
- 客户端发送部分数据后异常断连；
- 异常断连后重新建立连接并校验回显；
- 每条消息按长度收满，校验内容，不依赖一次 `read()` 返回完整消息。

默认参数：12 个并发客户端、每客户端 3 次重连、每连接 16 条随机消息。

## hiredis 场景

由 `example/redis/co_with_hiredis.cc` 使用 hiredis 1.1.0 连接本机 Redis，覆盖：

- 10 个协程池任务，每个任务复用一个独占 `redisContext`；
- 每个任务执行 1,000 次混合操作，总计 10,000 次写入/读取校验；
- 同步阻塞 `SET` / `GET`；
- 1–256 字节可打印随机值；
- `EXISTS` 存在性检查；
- 空字符串值；
- 随机比例 `DEL` 和删除后存在性检查；
- 每个任务独立 key，避免共享 context 造成假失败；
- 统计成功任务和操作错误，错误数必须为 0。

## 执行

```bash
python3 scripts/acceptance_real_clients.py \
  --echo-server ./build/bin/example/echo_server \
  --hiredis ./build/bin/example/co_with_hiredis
```

构建同时启用 `NEED_TEST=ON` 和 `NEED_EXAMPLE=ON` 时，CTest 注册
`Test_real_clients`。Redis 不可用时返回 CTest skip code 77，不伪造通过。

## 边界

该验收仍不覆盖 Redis 重启期间重连、服务端慢响应注入、完整协议 framing、
所有第三方客户端和任意 blocking FD 来源；这些场景需要独立客户端或故障注入。
