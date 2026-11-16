/**
 * @file __TCoSelect.hpp
 * @brief CoSelect 的模板与 inline 成员实现，由 CoSelect.hpp 末尾包含。
 */
#pragma once
#include <bbt/coroutine/sync/CoSelect.hpp>

namespace bbt::coroutine::sync {

template <class TItem, int Max> CoSelect& CoSelect::CaseRead(Chan<TItem, Max>& ch, TItem& out)
{
    static_assert(Max > 0, "CoSelect: Chan<T, 0> has no watcher API and is not supported");
    m_cases.push_back(Case{
        [&ch](const CoWaiter::SPtr& w) { ch.AddReadWatcher(w); },
        [&ch](const CoWaiter::SPtr& w) { ch.RemoveReadWatcher(w); },
        [&ch, &out]() { return ch.TryRead(out); },
    });
    return *this;
}

template <class TItem, int Max> CoSelect& CoSelect::CaseRead(std::shared_ptr<Chan<TItem, Max>> ch, TItem& out)
{
    return CaseRead(*ch, out);
}

template <class TItem, int Max> CoSelect& CoSelect::CaseWrite(Chan<TItem, Max>& ch, const TItem& val)
{
    static_assert(Max > 0, "CoSelect: Chan<T, 0> has no watcher API and is not supported");
    m_cases.push_back(Case{
        [&ch](const CoWaiter::SPtr& w) { ch.AddWriteWatcher(w); },
        [&ch](const CoWaiter::SPtr& w) { ch.RemoveWriteWatcher(w); },
        /**
         * val 按值捕获：调用方临时对象可能先于挂起期回调析构
         */
        [&ch, val]() { return ch.TryWrite(val); },
    });
    return *this;
}

template <class TItem, int Max> CoSelect& CoSelect::CaseWrite(std::shared_ptr<Chan<TItem, Max>> ch, const TItem& val)
{
    return CaseWrite(*ch, val);
}

inline CoSelect& CoSelect::CaseTimeout(int ms)
{
    m_timeout_ms = ms;
    return *this;
}

inline CoSelect& CoSelect::Default()
{
    m_has_default = true;
    return *this;
}

inline int CoSelect::_TryAll()
{
    for (size_t i = 0; i < m_cases.size(); ++i) {
        if (_IsReady(m_cases[i].try_op()))
            return (int)i;
    }
    return -1;
}

inline int CoSelect::Run()
{
    Assert(!m_cases.empty());

    auto waiter = CoWaiter::Create();

    /**
     * 1. 先全部 Try：有就绪立即返回，不挂起
     */
    int index = _TryAll();

    /**
     * 2. Default 优先于 Timeout：有 Default 且全未就绪立即 -1，不挂起
     */
    if (index < 0 && !m_has_default) {

        /**
         * 3. cb 在 Yield 之后执行：注册 watcher 后再 Try 一遍补丢唤醒窗口
         */
        int cb_index = -1;
        auto cb = [this, waiter, &cb_index]() {
            for (auto& c : m_cases)
                c.watch(waiter);
            cb_index = _TryAll();
            if (cb_index >= 0)
                waiter->Notify();
            return true;
        };

        if (m_timeout_ms >= 0)
            waiter->WaitWithTimeoutAndCallback(m_timeout_ms, cb);
        else
            waiter->WaitWithCallback(cb);

        /**
         * cb 在 Resume 线程上、协程重入前执行完毕（同线程顺序），cb_index 可见
         */
        if (cb_index < 0)
            index = _TryAll();
        else
            index = cb_index;
    }

    /**
     * 4. 无论命中/超时/Default 都清理：先摘 watcher 再 Cancel，迟到的 Notify 返回 -1
     */
    for (auto& c : m_cases)
        c.unwatch(waiter);
    waiter->Cancel();

    return index;
}

}
