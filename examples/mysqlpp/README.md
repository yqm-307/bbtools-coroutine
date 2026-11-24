# 在原生启动阶段执行阻塞式 MySQL 探针

[main.cc](main.cc) 用 `BBTCO_MAIN_WITH_HOOK` 把同步 `SELECT 1` 放在原生线程；探针成功才提交主协程，不把未经验证的 MySQL 阻塞调用带进 worker。

安装 MySQL 客户端开发包后构建。使用本地 `127.0.0.1:3306` 测试服务和已有的低权限用户，通过 `BBT_EXAMPLE_MYSQL_USER`、`BBT_EXAMPLE_MYSQL_PASSWORD` 传入配置。运行 `./build/examples/bin/examples/co_with_mysql`，依次输出 `native setup: SELECT 1 -> 1` 和 `main coroutine started`。不建表，不写数据。
