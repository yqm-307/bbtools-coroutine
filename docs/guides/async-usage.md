# Async 调用指南

先看 [可运行的 async 示例](../../examples/async/README.md)。常规用法是用 `bbt_co_async(T)` 显式声明可能挂起的操作，在受管协程里用 `bbt_co_await` 调用。完整契约见 [API 参考](api-reference.md#2-协程注册与调用语法)。

## 先记住这条规则

在定义处标出异步边界，在调用处标出等待点。顺序 async 函数体可以直接写多个 `bbt_co_await`，惰性操作仍使用 `bbt_co_bind`。

| 意图 | 写法 | 执行位置 | 是否等待最终结果 |
| --- | --- | --- | --- |
| 声明一个顺序 async 函数 | `bbt_co_async(T) Function(...)` + `bbt_co_return(...)` | 调用时开始执行 | 由调用方决定是否解包最终操作 |
| 当前协程等待操作（默认） | `bbt_co_await Function(...)` | 当前协程 | 是 |
| 显式提交惰性操作 | `bbt_co_fire bbt_co_bind(f, args...)` | `f` 在新 detached 协程执行 | 否 |
| 将已有函数或闭包连同参数打包，留待执行 | `bbt_co_bind(f, args...)` | 不调用 `f` | 不适用 |

顺序 async 函数在调用时执行函数体，`bbt_co_return(...)` 只包装最终结果。`bbt_co_bind` 创建的操作仍然惰性，只有 `bbt_co_await` 或 `bbt_co_fire` 才执行绑定的函数体。

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

惰性 async 操作也不能直接调用：

```cpp
auto operation = bbt_co_bind([] { return 42; }); // 此时不调用 lambda
operation();                  // 错误：AsyncOperation 没有业务调用运算符
```

## 定义异步操作

`bbt_co_async(T)` 是函数返回类型。C++17 不会自动改写函数体，因此顺序 async 函数需要用 `bbt_co_return(...)` 把最终业务值包装成 `AsyncOperation<T>`。函数体从调用开始执行，内部的 `bbt_co_await` 仍然可以挂起当前有栈协程。

```cpp
bbt_co_async(int) ReadValue();

bbt_co_async(int) ReadNextValue()
{
    const auto value = bbt_co_await ReadValue();
    bbt_co_return(value + 1);
}
```

`bbt_co_return(...)` 返回一个已经完成的 `AsyncOperation<int>`，调用方仍然写 `bbt_co_await ReadNextValue()` 来解包它。

`void` 函数使用 `bbt_co_return_void`，提前返回和异常仍然在函数调用时发生：

```cpp
bbt_co_async(void) Flush(int& calls)
{
    ++calls;
    bbt_co_return_void;
}

bbt_co_async(int) Check(bool valid)
{
    if (!valid) {
        bbt_co_return(-1);
    }
    bbt_co_return(1);
}
```

需要延迟执行、按值保存参数或提交 detached 操作时，继续使用 `bbt_co_bind`。顺序 async 函数不能依赖 `bbt_co_fire` 把函数体本身推迟到 detached 协程。`MakeAsyncOperation` 和 `bbt_co_async_body` 是兼容底层写法，新代码不用直接写它们。

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
    bbt_co_fire bbt_co_bind(RefreshMetrics);    // RefreshMetrics 是普通函数
    bbt_co_fire bbt_co_bind(PublishAuditEvent); // PublishAuditEvent 是普通函数
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

操作只能移动，只能消费一次。若 `LoadConfig()` 返回 `bbt_co_bind` 构造的操作，读取发生在 `await`；若它是顺序 async 函数，读取发生在调用时，保存的是已完成的结果。

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
StartRequest(); // 丢弃操作会触发 nodiscard 诊断；不能据此判断请求是否已启动
```

需要在当前协程等待请求时写：

```cpp
bbt_co_await StartRequest();
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
bbt_co_fire SaveRecord();        // 本例中 SaveRecord 返回惰性操作
bbt_co_await NotifyRecordSaved(); // 可能早于 SaveRecord 完成
```

有依赖时改为：

```cpp
bbt_co_await SaveRecord();
bbt_co_await NotifyRecordSaved();
```

## 写调用代码前的检查顺序

1. 常规调用写 `bbt_co_await Function(...)`，其中 `Function` 声明为 `bbt_co_async(T)`。确实需要把已有普通函数或 lambda 包装成惰性操作时才用 `bbt_co_bind`。
2. 只对惰性操作使用 `bbt_co_fire`，且调用方不能依赖其结果或完成顺序。
3. 确认调用发生在受管协程上下文中。普通线程或 native callback 不能直接使用 `bbt_co_await`。
4. 检查操作体跨越延迟点保存的数据，避免借用调用方栈和 `std::string_view` 指向的临时内容。
5. 将惰性实现改成顺序函数前，检查是否有调用方保存操作以推迟执行，或通过 `fire` 后台执行。若有，保留原入口的惰性实现；只保留签名不能保留执行时机。

## 语法细节

`bbt_co_await` 和 `bbt_co_fire` 使用 `->*` 表达式。需要继续访问成员或取反时加括号：

```cpp
auto first = (bbt_co_await ReadPair()).first;
if (!(bbt_co_await Ready())) {
    return;
}
```

不要嵌套裸写两个 `bbt_co_await`，也不要把宏当作普通函数传递。
