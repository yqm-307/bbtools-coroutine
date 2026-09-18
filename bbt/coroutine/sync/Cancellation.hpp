#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bbt::coroutine
{

class CancellationSource;
namespace sync { class CoWaiter; }   // 等待原语需向令牌登记唤醒回调（#347②）

namespace detail
{

/**
 * @brief 取消信号的共享内核：管理「是否已被要求取消」这一个标志，以及
 *        「取消到来时要叫醒谁」的那份回调表。
 *
 * 应用场景：一个协程在等远端结果（例如挂在 CompletionSignal 上等 RPC
 * 返回）。等待期间可能有别的来源要求它提前放弃——上游请求被断开、父
 * 请求预算到期、本次调用自己的超时、进程停机。有了取消，等待者不必干
 * 等到超时：等之前登记一个回调，取消一到就被叫醒并返回 Cancelled。
 *
 * 为什么要单独一个类：令牌是可自由拷贝的只读句柄，但「是否已取消」与
 * 「谁在等」必须全进程只有一份，所以状态从句柄里拆出来共享。
 * CancellationSource 是写端，CancellationToken 是只读视图，本类是被
 * 两者共享的内核。对外只有三件事：
 *  - Request()：置位并叫醒所有已登记者（幂等，仅首次生效）；
 *  - Register() / Unregister()：登记 / 撤销「取消时叫我」。
 */
class CancellationState
{
public:
    CancellationState() = default;

    bool                            IsRequested() const noexcept;
    /**
     * @brief 幂等置位；仅首次调用返回 true。
     * 回调统一在锁外执行，允许回调再进入其它 CancellationState。
     */
    bool                            Request() noexcept;
    /**
     * @brief 登记取消回调。
     * @return 非 0 表示登记号，可 Unregister；返回 0 表示未登记
     *         （已取消，回调已在调用线程同步执行）。
     */
    std::uint64_t                   Register(std::function<void()> cb);
    void                            Unregister(std::uint64_t id) noexcept;
private:
    std::atomic_bool                m_requested{false};
    std::mutex                      m_mtx;
    std::unordered_map<std::uint64_t, std::function<void()>>
                                    m_callbacks;
    std::uint64_t                   m_next_callback_id{1};
};

/**
 * @brief 组合 token 的「取消源 OR 视图」：持有一组源状态，任一源取消即视为取消。
 *
 * 视图自身没有可置位的取消状态，Combine 时不在源上登记任何传播回调，
 * 因此嵌套组合天然等价于扁平化的源集合，不依赖中间 token 是否被调用者
 * 保留（旧实现用 weak_ptr 做状态间传播，中间组合析构即断链）。
 * 等待登记把同一个回调登记到每个源上；登记簿记录「组号 → 各源侧登记号」
 * 供 Unregister 整组撤销。已取消的源由 CancellationState::Register 同步
 * 回调并返回 0，故同一回调可能被多个源各通知一次——重复通知由唤醒侧的
 * 事件状态机吸收，本视图不去重。
 */
class CancellationView
{
public:
    /* 仅供源数 >= 2 的组合使用；构造后源集合不再变化 */
    explicit                        CancellationView(
                                        std::vector<std::shared_ptr<CancellationState>> sources);

    bool                            IsRequested() const noexcept;
    /**
     * @brief 把同一回调登记到每个源。
     * @return 非 0 为本次登记的组号，可 Unregister 整组撤销；返回 0 表示
     *         无登记（全部源已取消，回调已同步派发）。
     */
    std::uint64_t                   Register(const std::function<void()>& cb);
    void                            Unregister(std::uint64_t id) noexcept;
    const std::vector<std::shared_ptr<CancellationState>>&
                                    Sources() const noexcept;
private:
    std::vector<std::shared_ptr<CancellationState>>
                                    m_sources;
    std::mutex                      m_mtx;
    /* 组号 → 各源上的 (源状态, 源侧登记号) */
    std::unordered_map<std::uint64_t,
        std::vector<std::pair<std::shared_ptr<CancellationState>, std::uint64_t>>>
                                    m_groups;
    std::uint64_t                   m_next_group_id{1};
};

} // namespace detail

/**
 * @brief 取消令牌：回答「这件事还要不要继续做」的只读句柄。
 *
 * 可默认构造、可自由拷贝；默认构造的令牌永不取消（不持有共享状态）。
 * 等待者拿一份，用 IsCancellationRequested() 查询，或在等待前登记叫醒
 * 回调。令牌自身改不了状态——取消只能由发起方经 CancellationSource 发出。
 *
 * Combine(parent, extra) 把两个来源合成一个令牌，任一取消即整体取消；
 * 典型用法是「父请求的预算」加「本次调用自己的超时」。组合结果是持有
 * 两侧源状态集合的只读 OR 视图（嵌套组合展开为扁平集合），不反向取消
 * 输入令牌，也不在源之间建传播链。
 */
class CancellationToken
{
public:
    CancellationToken() noexcept = default;
    bool                            IsCancellationRequested() const noexcept;
    static CancellationToken        Combine(CancellationToken parent, CancellationToken extra);
private:
    friend class CancellationSource;
    friend class sync::CoWaiter;
    friend class CancellationTokenTestAccess;
    explicit                        CancellationToken(std::shared_ptr<detail::CancellationState> state) noexcept;
    explicit                        CancellationToken(std::shared_ptr<detail::CancellationView> view) noexcept;

    /* 供等待者登记「取消时叫醒我」；已取消的令牌立即同步回调并返回 0。 */
    std::uint64_t                   _RegisterCancelCallback(std::function<void()> cb) const;
    void                            _UnregisterCancelCallback(std::uint64_t id) const noexcept;
    /* 把本令牌覆盖的源状态追加到 out（组合令牌展开为整个源集合） */
    void                            _CollectSources(
                                        std::vector<std::shared_ptr<detail::CancellationState>>& out) const;

    /* 单源令牌直接持有状态，组合令牌持 OR 视图；两者至多其一非空，
     * 皆空即默认构造的「永不取消」令牌。 */
    std::shared_ptr<detail::CancellationState>
                                    m_state{nullptr};
    std::shared_ptr<detail::CancellationView>
                                    m_view{nullptr};
};

/**
 * @brief 取消源：取消信号的写端，谁持有它谁就能发起取消。
 *
 * RequestCancel 幂等且线程安全。它只表达「不必等了」这个意图，不负责
 * 停止第三方 I/O——正在进行的网络操作是否真的中断，取决于 I/O 层自己是
 * 否响应取消。
 */
class CancellationSource
{
public:
    CancellationSource();
    CancellationToken               Token() const noexcept;
    void                            RequestCancel() noexcept;
private:
    std::shared_ptr<detail::CancellationState>
                                    m_state;
};

} // namespace bbt::coroutine
