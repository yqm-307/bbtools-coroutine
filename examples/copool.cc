#include <cstdio>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

/**
 * 尺寸路由池示例。
 *
 * 每个 worker 类创建时就固定栈规格 (U, protect)、worker 数与队列容量；
 * Submit 必须带同样的栈请求，池只把它交给精确匹配的类，不跨尺寸兜底。
 * 主任务也用显式默认栈，演示 RuntimeOptions 在 Start 前生效。
 */
RuntimeOptions PoolOptions() {
    RuntimeOptions options;
    options.default_stack = StackRequest{64 * 1024, true};
    options.stacks = StackConfig{
        StackPolicy::Strict,
        {{{12 * 1024, true}, 8}, {{64 * 1024, true}, 8}, {{128 * 1024, true}, 8}},
        0,
        0};
    return options;
}

int bbtco_main() {
    RequireCoroutineContext();
    printf("main task stack: %zu bytes\n", GetLocalCoroutineStackSize());

    auto pool = pool::CoPool::Create(pool::CoPoolOptions{
        {{{12 * 1024, true}, 2, 4}, {{64 * 1024, true}, 1, 2}, {{128 * 1024, true}, 1, 2}}});

    // 任务跑在类的 worker 栈上，读到的就是该类的规格。
    for (const auto& request : {StackRequest{12 * 1024, true},
                                StackRequest{64 * 1024, true},
                                StackRequest{128 * 1024, true}}) {
        const auto status = pool->Submit(request, [request]() {
            printf("task on %zu-byte stack (requested %zu)\n",
                   GetLocalCoroutineStackSize(), request.usable_bytes);
        });
        printf("submit %zu bytes: %s\n", request.usable_bytes,
               status == pool::CoPoolSubmitStatus::Accepted ? "accepted" : "rejected");
    }

    // 未声明的尺寸不会借到相邻类；提交立即拿到明确拒绝而不是排队等预算。
    const auto rejected = pool->Submit(StackRequest{32 * 1024, true}, []() {
        printf("this task must never run\n");
    });
    printf("submit undeclared 32 KiB: %s\n",
           rejected == pool::CoPoolSubmitStatus::NoExactClass ? "no exact class" : "unexpected");

    bbtco_sleep(100); // 让上面的任务先执行完，再演示关闭

    // Release 只能在本池 worker 之外调用；池内调用会抛 std::logic_error。
    pool->Release();
    printf("pool released\n");
    return 0;
}

BBTCO_MAIN_WITH_OPTIONS(PoolOptions())
