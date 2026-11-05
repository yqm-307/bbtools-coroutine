/** @file CoSelect.hpp @brief Go select 风格的多 Chan 复用。 */
#pragma once
#include <functional>
#include <vector>
#include <bbt/coroutine/sync/Chan.hpp>

namespace bbt::coroutine::sync {

/**
 * @brief Go select 风格多路 Chan 复用
 *
 * 用法：
 *   int idx = CoSelect().CaseRead(ch1, a).CaseWrite(ch2, v).CaseTimeout(100).Run();
 *   idx 为命中的 Case 注册序下标；-1 = timeout / Default / 唤醒后全未就绪。
 *   命中 CaseRead 时值写入对应 out，CaseWrite 成功即值已入队。
 *
 * 时序约束（防丢唤醒）：
 *   watcher 只在 WaitWithCallback 的回调里注册——回调在协程 Yield 之后
 *   （Processer::Resume 内）执行，唤醒方的 Notify 不可能早于挂起；注册后
 *   回调内立即再 Try 一遍，若恰好落在"Try 失败与注册之间"的窗口则
 *   waiter->Notify() 自唤醒。
 *   唤醒后被别的 reader/writer 抢走数据属于合法竞争，此时 Run 返回 -1，
 *   调用方像 Go 一样外层循环即可。
 *
 * 生命周期：引用重载要求 Chan 与 out 存活到 Run() 返回（典型同栈用法）；
 * SPtr 重载内部转 *ch，临时 shared_ptr 只在该语句内保活，跨语句使用需
 * 调用方自行持有。
 *
 * 仅支持 Max>0 的缓冲 Chan：Chan<T,0> 为 private 继承、watcher 接口不可见，
 * Case* 对 Max==0 在编译期 static_assert 拒绝。
 */
class CoSelect {
public:
    /// @brief 添加一个缓冲 Chan 的读分支；命中时把值写入 out。
    /// @tparam TItem Chan 中的元素类型
    /// @tparam Max 缓冲容量，必须大于 0
    /// @param ch 借用的 Chan，须存活至 Run() 返回
    /// @param out 命中时接收读出的值
    template <class TItem, int Max> CoSelect& CaseRead(Chan<TItem, Max>& ch, TItem& out);

    /// @brief 添加一个 shared_ptr 参数的缓冲 Chan 读分支；Chan 须由调用方保活至 Run() 返回。
    /// @tparam TItem Chan 中的元素类型
    /// @tparam Max 缓冲容量，必须大于 0
    /// @param ch 指向需由调用方保活至 Run() 返回的 Chan
    /// @param out 命中时接收读出的值
    template <class TItem, int Max> CoSelect& CaseRead(std::shared_ptr<Chan<TItem, Max>> ch, TItem& out);

    /// @brief 添加一个缓冲 Chan 的写分支；命中表示 val 已入队。
    /// @tparam TItem Chan 中的元素类型
    /// @tparam Max 缓冲容量，必须大于 0
    /// @param ch 借用的 Chan，须存活至 Run() 返回
    /// @param val 待写入的值；注册时按值捕获
    template <class TItem, int Max> CoSelect& CaseWrite(Chan<TItem, Max>& ch, const TItem& val);

    /// @brief 添加一个 shared_ptr 参数的缓冲 Chan 写分支；Chan 须由调用方保活至 Run() 返回。
    /// @tparam TItem Chan 中的元素类型
    /// @tparam Max 缓冲容量，必须大于 0
    /// @param ch 指向需由调用方保活至 Run() 返回的 Chan
    /// @param val 待写入的值；注册时按值捕获
    template <class TItem, int Max> CoSelect& CaseWrite(std::shared_ptr<Chan<TItem, Max>> ch, const TItem& val);

    /// @brief 设置等待超时；与 Default 同时出现时 Default 优先（不挂起）。
    /// @param ms 超时毫秒；负值表示不设超时（默认）
    /// @return 当前 CoSelect，供链式调用
    CoSelect& CaseTimeout(int ms);

    /// @brief 设置非阻塞默认分支；全部未就绪时立即返回 -1。
    /// @return 当前 CoSelect，供链式调用
    CoSelect& Default();

    /// @brief 在协程上下文中按分支注册顺序尝试各分支，必要时挂起等待。
    /// @return 命中分支的下标；超时、默认分支或恢复后仍未就绪时返回 -1。
    int Run();

private:
    /* type erasure：watch/unwatch 操作对应 Chan 的 watcher 列表（Chan 内部持锁），
     * try_op 返回 TryRead/TryWrite 的原始返回值 */
    struct Case {
        std::function<void(const CoWaiter::SPtr&)> watch;
        std::function<void(const CoWaiter::SPtr&)> unwatch;
        std::function<int()> try_op;
    };

    /* 0 成功；-1 Chan 已关闭（永远等不到更多，视为就绪）。-2 = 未就绪 */
    static bool _IsReady(int ret)
    {
        return ret == 0 || ret == -1;
    }

    int _TryAll();

    std::vector<Case> m_cases;
    int m_timeout_ms{-1};
    bool m_has_default{false};
};

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
        /* val 按值捕获：调用方临时对象可能先于挂起期回调析构 */
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

    /* 1. 先全部 Try：有就绪立即返回，不挂起 */
    int index = _TryAll();

    /* 2. Default 优先于 Timeout：有 Default 且全未就绪立即 -1，不挂起 */
    if (index < 0 && !m_has_default) {
        /* 3. cb 在 Yield 之后执行：注册 watcher 后再 Try 一遍补丢唤醒窗口 */
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

        /* cb 在 Resume 线程上、协程重入前执行完毕（同线程顺序），cb_index 可见 */
        if (cb_index < 0)
            index = _TryAll();
        else
            index = cb_index;
    }

    /* 4. 无论命中/超时/Default 都清理：先摘 watcher 再 Cancel，迟到的 Notify 返回 -1 */
    for (auto& c : m_cases)
        c.unwatch(waiter);
    waiter->Cancel();

    return index;
}

} // namespace bbt::coroutine::sync
