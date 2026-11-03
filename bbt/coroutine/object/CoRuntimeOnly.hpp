/* Co-only 能力标记：仅显式入口检查，不约束对象构造、析构或句柄移动。 */
#pragma once

namespace bbt::coroutine {

bool IsInCoroutineContext();
void RequireCoroutineContext();

class CoRuntimeOnly {
public:
    static bool IsInCoroutineContext() { return bbt::coroutine::IsInCoroutineContext(); }
    static void RequireCoroutineContext() { bbt::coroutine::RequireCoroutineContext(); }
};

} // namespace bbt::coroutine
