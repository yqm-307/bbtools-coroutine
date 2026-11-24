# Async 调用指南

常规用法是用 `bbt_co_async(T)` 显式声明可能挂起的操作，在受管协程里用 `bbt_co_await` 调用。完整契约见 [API 参考](api-reference.md#2-协程注册与调用语法)。

## 先记住这条规则

像 Python 的 `async` / `await` 一样，在定义处标出异步边界，在调用处标出等待点。不要把带 `bbt_co_await` 的业务流程提取成返回普通值的命名函数，再交给外层 `bind`；这样从签名看不出它可能挂起。

| 意图 | 写法 | 执行位置 | 是否等待最终结果 |
| --- | --- | --- | --- |
| 声明一个可能挂起的操作 | `bbt_co_async(T) Function(...)` | 声明本身不执行 | 不适用 |
| 当前协程等待操作（默认） | `bbt_co_await Function(...)` | 当前协程 | 是 |
| 显式提交 detached 操作 | `bbt_co_fire Function(...)` | 新 detached 协程 | 否 |
| 将已有函数或闭包连同参数打包，留待执行 | `bbt_co_bind(f, args...)` | 不调用 `f` | 不适用 |

`bbt_co_await` 和 `bbt_co_fire` 是执行入口。直接调用异步函数只构造惰性操作对象，不执行其中的操作体。

```cpp
bbt_co_async(int) ReadValue();

// 在 runtime 启动后注册协程。
void StartRead()
{
    bbtco [] {
        const auto value = bbt_co_await ReadValue();
        (void)value;
    };
}
```

普通值没有匹配的 helper。下面两种写法都应在编译期失败：

```cpp
int ReadValue();

bbt_co_await ReadValue(); // 错误：普通 int 不是 async 操作
bbt_co_fire ReadValue();  // 错误：普通 int 不是 async 操作
```

async 操作也不能直接调用：

```cpp
auto operation = ReadValue(); // 只构造操作描述
operation();                  // 错误：AsyncOperation 没有业务调用运算符
```

## 定义异步操作

与 Python 不同，C++17 的 `bbt_co_async(T)` 只是返回类型，不会自动延迟 C++ 函数体。定义必须返回一个惰性操作；需要用 `bbt_co_bind` 构造它时，把挂起逻辑留在标有 `bbt_co_async` 的函数定义内，用局部闭包作为执行体。只做计算或数据转换、不挂起的辅助函数，可以照常返回普通值。

```cpp
bbt_co_async(int) ReadValue();

bbt_co_async(int) ReadNextValue()
{
    return bbt_co_bind([] {
        const auto value = bbt_co_await ReadValue();
        return value + 1;
    });
}
```

这里的 `bind` 只负责构造操作；调用方仍然写 `bbt_co_await ReadNextValue()`。

从旧版迁移时，保留原有的 `async` 签名和 `await` / `fire` 调用点。原先直接返回 `T` 的定义需要显式构造惰性操作，把原来应该在消费时执行的逻辑放进执行体，而不是在构造操作时提前做。`MakeAsyncOperation` 和 `bbt_co_async_body` 是兼容底层写法，新代码不用直接写它们。

## 常见调用场景

### 有顺序依赖时使用 `await`

```cpp
bbtco [] {
    auto token = bbt_co_await FetchToken();
    auto response = bbt_co_await SendRequest(std::move(token));
    (void)response;
};
```

第二个操作依赖第一个结果，必须等待第一个操作完成。

### 独立的 detached 副作用使用 `fire`

```cpp
bbtco [] {
    bbt_co_fire RefreshMetrics();
    bbt_co_fire PublishAuditEvent();
};
```

`fire` 放弃最终返回值。提交失败和 detached 操作的异常仍按运行时错误处理契约收口。需要观察结果、错误或完成时刻时使用 `await`。

### 保存操作后显式移动消费

```cpp
bbtco [] {
    auto operation = LoadConfig();
    auto config = bbt_co_await std::move(operation);
    (void)config;
};
```

操作是 move-only 且只能消费一次。保存操作不会执行它。

### 确实需要打包函数或闭包时使用 `bind`

例如框架需要接收一个待执行的操作，而手头只有回调和参数。用 `bbt_co_bind` 把它们包装成操作对象；对已经返回 `bbt_co_async(T)` 的函数，直接取得其操作，不要再绑一层。

```cpp
void PublishValue(int value);

bbt_co_async(void) MakePublishOperation(int value)
{
    return bbt_co_bind(PublishValue, value);
}
```

这里假定 `PublishValue(int)` 是已有的普通 `void` 回调。`bind` 在构造时保存参数，不调用回调；回调在 `await` 或 `fire` 消费时才执行。

## 不建议的写法

### 不要把 async 操作当同步函数调用

```cpp
StartRequest(); // 只构造操作，不会启动请求
```

需要执行时明确写：

```cpp
bbt_co_await StartRequest();
// 或
bbt_co_fire StartRequest();
```

### 不要用引用捕获跨越未知的执行寿命

```cpp
bbt_co_async(void) Bad(std::string_view name)
{
    return bbt_co_bind([&] {
        Use(name); // 错误风险：name 或其指向内容可能已经失效
    });
}
```

把需要跨越延迟点的数据复制成拥有所有权的值，或捕获明确的 `shared_ptr`。

确实需要访问外部对象时，可以用 `std::ref` / `std::cref` 显式借用，并保证它活过操作。若返回引用，还要保证引用在 `await` 返回后有效，不能指向 `bind` 保存的参数副本或 lambda 自己的成员。

### 不要用 `fire` 表达依赖关系

```cpp
bbt_co_fire SaveRecord();
bbt_co_await NotifyRecordSaved(); // 可能早于 SaveRecord 完成
```

有依赖时改为：

```cpp
bbt_co_await SaveRecord();
bbt_co_await NotifyRecordSaved();
```

## 写调用代码前的检查顺序

1. 常规调用写 `bbt_co_await Function(...)`，其中 `Function` 声明为 `bbt_co_async(T)`。确实需要把已有普通函数或 lambda 包装成惰性操作时才用 `bbt_co_bind`。
2. 仅在明确放弃最终结果且没有完成顺序依赖时，用 `bbt_co_fire` 代替 `await`。
3. 确认调用发生在受管协程上下文中。普通线程或 native callback 不能直接使用 `bbt_co_await`。
4. 检查操作体跨越延迟点保存的数据，避免借用调用方栈和 `std::string_view` 指向的临时内容。
5. 迁移时保留 async 签名和现有调用方的 `await` / `fire` 形式，不要把可能挂起的函数改成普通返回值。

## 语法细节

`bbt_co_await` 和 `bbt_co_fire` 使用 `->*` 表达式。需要继续访问成员或取反时加括号：

```cpp
auto first = (bbt_co_await ReadPair()).first;
if (!(bbt_co_await Ready())) {
    return;
}
```

不要嵌套裸写两个 `bbt_co_await`，也不要把宏当作普通函数传递。
