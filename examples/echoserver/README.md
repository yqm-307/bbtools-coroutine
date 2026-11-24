# 本机 TCP Echo

[服务端](server.cc) 只监听 `127.0.0.1:10010`，每连接交给一个协程。服务端处理短写；[客户端](client.cc) 按字节数收完回显，不假设一次 `recv` 就能读完。fd 随作用域关闭，注册失败时由接收方立即关闭。

在两个终端分别运行 `./build/examples/bin/examples/echo_server` 和 `./build/examples/bin/examples/echo_client`。客户端输出 `echo: hello` 后退出，服务端用 `Ctrl+C` 停止。这是本机示例，不是生产服务模板；没有连接数限制或优雅停机协议。
