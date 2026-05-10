## 1. TryRead 资源泄露修复 (chan-resource-cleanup)

- [x] 1.1 修复 TryRead(item) 队列为空返回 -1 路径：添加 m_is_reading = false 和 _UnLock()
- [x] 1.2 修复 TryRead(item) 成功路径：在 _UnLock() 前添加 m_is_reading = false
- [x] 1.3 修复 TryRead(item, timeout) 超时后队列仍空返回 -1 路径：添加 m_is_reading = false 和 _UnLock()
- [x] 1.4 修复 TryRead(item, timeout) _WaitUntilEnableReadOrTimeout 返回 -1 路径：添加 m_is_reading = false

## 2. Read/ReadAll 资源泄露修复 (chan-resource-cleanup)

- [x] 2.1 修复 Read(item) _WaitUntilEnableRead 返回 -1 路径：添加 m_is_reading = false
- [x] 2.2 修复 Read(item) 被唤醒后 _Lock() 再检查队列空/IsClosed 的 return -1 路径：添加 m_is_reading = false 和 _UnLock()
- [x] 2.3 修复 ReadAll(items) _WaitUntilEnableRead 返回 -1 路径：添加 m_is_reading = false

## 3. Write/Close 安全修复 (chan-close-write-safety)

- [x] 3.1 修复 Write(item)：被唤醒后在 _Lock() 前添加 IsClosed() 检查，返回 -1
- [x] 3.2 修复 Write(item)：被唤醒后 _Lock() 后添加队列容量检查，队列满时 _UnLock() 并返回 -1
- [x] 3.3 修复 TryWrite(item, timeout)：_WaitUntilEnableWriteOrTimeout 返回非 0 时直接返回，不执行 push
- [x] 3.4 修复 TryWrite(item, timeout)：等待成功后添加 IsClosed() 检查
- [x] 3.5 修复 TryWrite(item, timeout)：等待成功后 _Lock() 后添加队列容量检查

## 4. Chan<T,0> API 可见性修复 (chan-unbuffered-api)

- [x] 4.1 将 Chan<T,0> 对 Chan<T,1> 的继承方式从 protected 改为 private
- [x] 4.2 在 Chan<T,0> 的 public 区域添加 using 声明暴露 TryRead、TryWrite、ReadAll、IsClosed

## 5. 验证

- [x] 5.1 编译全部代码无 error 无 warning
- [x] 5.2 运行全部单元测试，确认 56/56 通过（含之前标记为缺陷的 #23 #24）
- [x] 5.3 验证 Chan<T,0> 的 TryRead/TryWrite/ReadAll 在测试中可编译调用
