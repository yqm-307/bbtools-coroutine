# Async 调用指南

本文介绍如何用 `bbt_co_async`、`bbt_co_bind`、`bbt_co_await` 和 `bbt_co_fire` 声明、构造和执行异步操作。完整契约见 [API 参考](api-reference.md#2-协程注册与调用语法)。

## 先记住这条规则

可能挂起的操作必须通过明确的 async 操作类型表达，并通过匹配的语法糖执行。

| 意图 | 写法 | 执行位置 | 是否等待最终结果 |
| --- | --- | --- | --- |
| 声明一个可能挂起的操作 | `bbt_co_async(T) Function(...)` | 不执行函数体 | 不适用 |
| 把普通函数或 lambda 绑定为待执行操作 | `bbt_co_bind(f, args...)` | 不调用 `f` | 不适用 |
| 当前协程等待操作 | `bbt_co_await Function(...)` | 当前协程 | 是 |
| 显式提交 detached 操作 | `bbt_co_fire Function(...)` | 新 detached 协程 | 否 |

`bbt_co_await` 和 `bbt_co_fire` 是调用入口。普通函数调用只构造惰性操作对象，不执行操作体。

```cpp
bbt_co_async(int) ReadValue()
{
    return bbt_co_bind([] { return 42; });
}

bbtco [] {
    const auto v1 = bbt_co_await ReadValue();
    const auto value = bbt_co_await bbt_co_bind([](int x) { return x; }, v1);
    (void)value;
};
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

## 推荐的实现结构

短操作直接用 lambda。较长的操作用一个普通的命名函数写主体，async 入口只绑定函数和参数，不再重复写操作体返回类型。

```cpp
static result<Response> SendBody(
    std::shared_ptr<http::Client> http,
    std::string target,
    std::string body)
{
    http::RequestOptions options;
    options.headers.emplace_back("Accept", "application/json");

    auto request = http->BuildRequest("POST", target, options);
    if (!request) {
        return result<Response>::err(std::move(request.error()));
    }
    request.value().body = std::move(body);

    return bbt_co_await http->Send(request.value());
}

bbt_co_async(result<Response>) Send(std::string target, std::string body) const
{
    return bbt_co_bind(SendBody, http, std::move(target), std::move(body));
}
```

这样做有三个目的：

- `Send` 的公开返回类型仍是 `AsyncOperation<T>`，所以 `await` 和 `fire` 的调用边界不变。
- 参数在操作对象里取得所有权：入口在调用点复制或移动参数，操作体执行时看到的是当时的快照，不借用调用方的栈，也不读取调用方之后修改的对象；需要共享寿命时捕获明确的 `shared_ptr`。
- 请求准备、错误处理和 `bbt_co_await` 保持左到右的普通函数体结构，阅读业务逻辑时不需要先穿过一整层 lambda。

已返回 `bbt_co_async(T)` 的函数直接 `await` 或 `fire`，不再套 `bind`。`MakeAsyncOperation` 和 `bbt_co_async_body` 保留给已有底层写法；新代码不用直接写它们。

从旧版迁移时，原先直接返回 `T` 的 `bbt_co_async(T)` 定义需要改写：公开入口改为返回 `bbt_co_bind(...)`，原来的函数体搬到被绑定的普通函数或 lambda 中。已有 `bbt_co_await Function(...)` 调用不需要改写；直接调用现在只构造操作，不再同步执行主体。

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

### `void` 操作使用普通 `void` 函数体

```cpp
bbt_co_async(void) Flush()
{
    return bbt_co_bind([] {
        bbtco_sleep(1);
    });
}
```

`bind` 推导出 `void`，lambda 不必返回任何标记。

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

### 不要把整段业务逻辑塞进长 lambda

```cpp
bbt_co_async(result<Response>) Send(...) {
    return bbt_co_bind([/* 明确的捕获 */]() -> result<Response> {
        // 很长的请求准备、校验、重试和资源收口逻辑
    });
}
```

这种写法会把延迟机制和业务逻辑混在一起，也容易隐藏引用捕获和参数所有权问题。保留薄入口，把主体放到命名的 `Body` 函数中。

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

1. 已经返回 `bbt_co_async(T)` 的函数直接调用；要把普通函数或 lambda 变成惰性操作时使用 `bbt_co_bind`。
2. 判断当前操作是否需要结果、错误或完成时序。需要就用 `bbt_co_await`，明确放弃最终结果才用 `bbt_co_fire`。
3. 确认调用发生在受管协程上下文中。普通线程或 native callback 不能直接使用 `bbt_co_await`。
4. 检查操作体跨越延迟点保存的数据，避免借用调用方栈和 `std::string_view` 指向的临时内容。
5. 保持现有调用方的 `await` / `fire` 形式。迁移实现时优先收窄 async 入口，不要把 async API 改回普通返回值。

## 语法细节

`bbt_co_await` 和 `bbt_co_fire` 使用 `->*` 表达式。需要继续访问成员或取反时加括号：

```cpp
auto first = (bbt_co_await ReadPair()).first;
if (!(bbt_co_await Ready())) {
    return;
}
```

不要嵌套裸写两个 `bbt_co_await`，也不要把宏当作普通函数传递。
