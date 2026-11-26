/**
 * @file CoSelect.hpp
 * @brief Go select 风格的多 Chan 复用。
 */
#pragma once
#include <functional>
#include <memory>
#include <random>
#include <vector>
#include <bbt/coroutine/sync/Chan.hpp>

namespace bbt::coroutine::sync {

/**
 * @brief 从缓冲和零缓冲 Chan 的多个分支中选择至多一个。
 *
 * 用法：CoSelect().CaseRead(ch1, a).CaseWrite(ch2, v).CaseTimeout(100).Run()。
 * 关闭视为分支就绪，不读出缓冲残留，也不修改读出参数。零缓冲分支不能与同一 Run 的分支配对。
 * 读出赋值在 Run 调用方执行，赋值异常原样抛出。缓冲写在接纳时完成；零缓冲阻塞写等待读方
 * 赋值回执，交付失败返回 -1，不改选其他分支。已配对的分支不会被随后发生的超时或取消撤回。
 */
class CoSelect {
public:
    /**
     * @brief 添加一个读分支；命中且未关闭时把值写入 out。
     *
     * @param ch 通道，注册期间必须存活；分支持有通道的内部状态，Chan 析构仍会关闭通道
     * @param out 命中时接收读出的值，须存活至 Run() 返回
     */
    template <class TItem, int Max> CoSelect& CaseRead(Chan<TItem, Max>& ch, TItem& out);

    /**
     * @brief 添加一个读分支，不延长 Chan 对象的寿命。
     *
     * @param ch 指向通道的 shared_ptr
     * @param out 命中时接收读出的值，须存活至 Run() 返回
     */
    template <class TItem, int Max> CoSelect& CaseRead(std::shared_ptr<Chan<TItem, Max>> ch, TItem& out);

    /**
     * @brief 添加一个写分支。
     *
     * @param ch 通道，注册期间必须存活；分支持有通道的内部状态，Chan 析构仍会关闭通道
     * @param val 待写入的值；添加分支时按值保存
     */
    template <class TItem, int Max> CoSelect& CaseWrite(Chan<TItem, Max>& ch, const TItem& val);

    /**
     * @brief 添加一个写分支，不延长 Chan 对象的寿命。
     *
     * @param ch 指向通道的 shared_ptr
     * @param val 待写入的值；添加分支时按值保存
     */
    template <class TItem, int Max> CoSelect& CaseWrite(std::shared_ptr<Chan<TItem, Max>> ch, const TItem& val);

    /**
     * @brief 设置初次尝试全部未就绪后的等待超时；与 Default 同时出现时 Default 优先。
     *
     * @param ms 超时毫秒；0 不等待分支就绪，负值表示不设超时（默认）
     * @return 当前 CoSelect，供链式调用
     */
    CoSelect& CaseTimeout(int ms);

    /**
     * @brief 使用非阻塞选择；全部未就绪时立即返回 -1。
     *
     * 零缓冲写与无超时 TryWrite 一样，只确认已配对，不等待读方赋值回执。
     *
     * @return 当前 CoSelect，供链式调用
     */
    CoSelect& Default();

    /**
     * @brief 在协程中按注册分支尝试，多个同时就绪分支使用伪随机顺序选择，必要时挂起以等待首胜。
     *
     * @return 首胜分支的下标；默认分支、取消、超时、等待或交付失败返回 -1。
     */
    int Run();

private:
    using Wait = std::shared_ptr<detail::WaitNode>;

    struct Branch {
        virtual ~Branch() = default;
        virtual bool Probe(detail::Coroutine* co, bool immediate) = 0;
        virtual bool PairWithReceipt(detail::Coroutine* co, const Wait& wait) = 0;
        virtual void PrepareToPublish() = 0;
        virtual void Publish(detail::Coroutine* co, const Wait& wait) = 0;
        virtual bool Complete(bool immediate) = 0;
    };

    template <class TItem, int Max> class ChanBranch;

    using Case = std::function<std::unique_ptr<Branch>(const std::shared_ptr<SelectChoice>&, int)>;
    static std::mt19937& _RandomEngine();

    std::vector<Case> m_cases;
    int m_timeout_ms{-1};
    bool m_has_default{false};
};

} // namespace bbt::coroutine::sync

#include <bbt/coroutine/sync/__TCoSelect.hpp>
