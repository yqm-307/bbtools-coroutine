# 用 RAII 保护共享状态

[main.cc](main.cc) 用 `CoLockGuard` 在两条协程间保护计数，并用读锁、写锁在不同作用域访问快照。不要持读锁升级到写锁。

运行 `./build/examples/bin/examples/comutex`，输出 `snapshot: 3`。
