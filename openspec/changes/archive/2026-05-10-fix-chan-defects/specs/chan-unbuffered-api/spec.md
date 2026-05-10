## ADDED Requirements

### Requirement: Chan<T,0> 必须暴露 TryRead 方法
系统 SHALL 使 `Chan<T,0>` 的 `TryRead(item)` 和 `TryRead(item, timeout)` 方法对外部可访问。

#### Scenario: 无缓冲 Chan TryRead 可调用
- **WHEN** 创建 `Chan<int, 0>` 实例并调用 TryRead(item)
- **THEN** SHALL 编译通过且可正常调用，返回 0 或 -1

### Requirement: Chan<T,0> 必须暴露 TryWrite 方法
系统 SHALL 使 `Chan<T,0>` 的 `TryWrite(item)` 和 `TryWrite(item, timeout)` 方法对外部可访问。

#### Scenario: 无缓冲 Chan TryWrite 可调用
- **WHEN** 创建 `Chan<int, 0>` 实例并调用 TryWrite(item)
- **THEN** SHALL 编译通过且可正常调用，返回 0 或 -1

### Requirement: Chan<T,0> 必须暴露 ReadAll 方法
系统 SHALL 使 `Chan<T,0>` 的 `ReadAll(items)` 方法对外部可访问。

#### Scenario: 无缓冲 Chan ReadAll 可调用
- **WHEN** 创建 `Chan<int, 0>` 实例并调用 ReadAll(items)
- **THEN** SHALL 编译通过且可正常调用

### Requirement: Chan<T,0> 继承方式变更为 private + 显式 using
系统 SHALL 将 `Chan<T,0>` 对 `Chan<T,1>` 的继承方式从 `protected` 改为 `private`，并通过 `using` 声明显式暴露 TryRead、TryWrite、ReadAll、IsClosed 方法。

#### Scenario: 仅暴露的方法可访问
- **WHEN** 尝试通过 `Chan<int, 0>` 实例调用 `Write`、`Read`、`Close`、`TryRead`、`TryWrite`、`ReadAll`、`IsClosed`
- **THEN** 所有列出的方法 SHALL 可访问且编译通过
