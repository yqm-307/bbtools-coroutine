## ADDED Requirements

### Requirement: 有缓冲 Chan 不同元素类型读写正确性
系统 SHALL 支持 Chan 使用非平凡类型（如 std::string、自定义 struct）作为元素类型，且读写值一致。

#### Scenario: string 类型元素读写
- **WHEN** 创建 `Chan<std::string, 10>` 并写入 "hello"
- **THEN** 读取值 SHALL 等于 "hello"

#### Scenario: 自定义 struct 元素读写
- **WHEN** 创建 `Chan<TestStruct, 10>` 并写入包含 x=42, y=3.14 的结构体
- **THEN** 读取值的 x 和 y SHALL 分别等于 42 和 3.14

### Requirement: 有缓冲 Chan ReadAll 功能
系统 SHALL 提供 ReadAll 方法一次性读出缓冲队列中的全部元素，当队列为空时阻塞等待直到有可读数据。

#### Scenario: 队列非空时 ReadAll
- **WHEN** 向容量为 10 的 Chan 写入 3 个元素后调用 ReadAll
- **THEN** SHALL 返回 0 并在输出 vector 中包含 3 个元素，顺序与写入一致

#### Scenario: 队列为空时 ReadAll 阻塞
- **WHEN** 队列为空时调用 ReadAll，200ms 后另一协程写入 2 个元素
- **THEN** ReadAll SHALL 在写入后返回，输出 vector 包含 2 个元素

#### Scenario: ReadAll 读空后再次写入再 ReadAll
- **WHEN** ReadAll 读出全部元素后，再写入新元素并再次 ReadAll
- **THEN** 第二次 ReadAll SHALL 正确读出新的元素

### Requirement: 无缓冲 Chan ReadAll 行为
系统 SHALL 记录 `Chan<T,0>` 的 ReadAll 行为（继承自父类）。

#### Scenario: 无缓冲 Chan ReadAll
- **WHEN** 创建 `Chan<int, 0>` 并写入后调用 ReadAll
- **THEN** ReadAll SHALL 返回结果（记录实际行为，不预设预期）
