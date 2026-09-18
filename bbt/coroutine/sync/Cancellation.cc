#include <bbt/coroutine/sync/Cancellation.hpp>

namespace bbt::coroutine
{

namespace detail
{

bool CancellationState::IsRequested() const noexcept
{
    return m_requested.load(std::memory_order_acquire);
}

bool CancellationState::Request() noexcept
{
    if (m_requested.exchange(true, std::memory_order_acq_rel))
        return false;

    std::unordered_map<std::uint64_t, std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        callbacks.swap(m_callbacks);
    }

    /* 锁外派发：回调可能触发其它 CancellationState 的 Request/Unregister，
     * 持锁调用会把无关状态机的互斥串成环。单个回调失败不阻断其余传播。 */
    for (auto& item : callbacks) {
        try {
            item.second();
        } catch (...) {
        }
    }
    return true;
}

std::uint64_t CancellationState::Register(std::function<void()> cb)
{
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_requested.load(std::memory_order_acquire)) {
            const auto id = m_next_callback_id++;
            m_callbacks.emplace(id, std::move(cb));
            return id;
        }
    }

    /* 已取消：锁外立即回调，返回 0 表示无登记可解绑 */
    try {
        cb();
    } catch (...) {
    }
    return 0;
}

void CancellationState::Unregister(std::uint64_t id) noexcept
{
    if (id == 0)
        return;

    std::lock_guard<std::mutex> lock(m_mtx);
    m_callbacks.erase(id);
}

CancellationView::CancellationView(
    std::vector<std::shared_ptr<CancellationState>> sources):
    m_sources(std::move(sources))
{
}

bool CancellationView::IsRequested() const noexcept
{
    for (auto& s : m_sources) {
        if (s->IsRequested())
            return true;
    }
    return false;
}

std::uint64_t CancellationView::Register(const std::function<void()>& cb)
{
    std::vector<std::pair<std::shared_ptr<CancellationState>, std::uint64_t>> subs;
    subs.reserve(m_sources.size());

    try {
        /* 已取消的源 Register 立即同步回调并返回 0：登记簿只记真正登记
         * 成功的 (源, 登记号)，已同步派发的通知无需也无法撤销。 */
        for (auto& s : m_sources) {
            const auto id = s->Register(cb);
            if (id != 0)
                subs.emplace_back(s, id);
        }
        if (subs.empty())
            return 0;

        std::lock_guard<std::mutex> lock(m_mtx);
        const auto group_id = m_next_group_id++;
        /* 先取槽位再 move：若分配失败 subs 仍完整，catch 可回滚已登记项 */
        m_groups[group_id] = std::move(subs);
        return group_id;
    } catch (...) {
        for (auto& sub : subs)
            sub.first->Unregister(sub.second);
        throw;
    }
}

void CancellationView::Unregister(std::uint64_t id) noexcept
{
    if (id == 0)
        return;

    std::vector<std::pair<std::shared_ptr<CancellationState>, std::uint64_t>> subs;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        auto it = m_groups.find(id);
        if (it == m_groups.end())
            return;
        subs = std::move(it->second);
        m_groups.erase(it);
    }

    /* 先摘登记簿再逐个解绑：不在本锁内调用 CancellationState，互斥不成环 */
    for (auto& sub : subs)
        sub.first->Unregister(sub.second);
}

const std::vector<std::shared_ptr<CancellationState>>&
CancellationView::Sources() const noexcept
{
    return m_sources;
}

} // namespace detail

CancellationToken::CancellationToken(std::shared_ptr<detail::CancellationState> state) noexcept:
    m_state(std::move(state))
{
}

CancellationToken::CancellationToken(std::shared_ptr<detail::CancellationView> view) noexcept:
    m_view(std::move(view))
{
}

bool CancellationToken::IsCancellationRequested() const noexcept
{
    if (m_state != nullptr)
        return m_state->IsRequested();
    return m_view != nullptr && m_view->IsRequested();
}

void CancellationToken::_CollectSources(
    std::vector<std::shared_ptr<detail::CancellationState>>& out) const
{
    if (m_state != nullptr)
        out.push_back(m_state);
    if (m_view != nullptr) {
        const auto& srcs = m_view->Sources();
        out.insert(out.end(), srcs.begin(), srcs.end());
    }
}

CancellationToken CancellationToken::Combine(CancellationToken parent, CancellationToken extra)
{
    /* OR 视图：嵌套组合展开为扁平的源集合，任一源取消即整体取消，
     * 传播不依赖中间 token 的寿命。同一源重复出现不去重——重复登记
     * 产生的重复通知由唤醒侧状态机吸收。 */
    std::vector<std::shared_ptr<detail::CancellationState>> sources;
    parent._CollectSources(sources);
    extra._CollectSources(sources);

    if (sources.empty())
        return CancellationToken{};                 /* 双侧默认：永不取消 */
    if (sources.size() == 1)
        return CancellationToken(std::move(sources.front()));
    return CancellationToken(
        std::make_shared<detail::CancellationView>(std::move(sources)));
}

std::uint64_t CancellationToken::_RegisterCancelCallback(std::function<void()> cb) const
{
    if (m_state != nullptr)
        return m_state->Register(std::move(cb));
    if (m_view != nullptr)
        return m_view->Register(cb);
    return 0;   /* 默认 token 永不取消：无登记必要 */
}

void CancellationToken::_UnregisterCancelCallback(std::uint64_t id) const noexcept
{
    if (id == 0)
        return;
    if (m_state != nullptr)
        m_state->Unregister(id);
    else if (m_view != nullptr)
        m_view->Unregister(id);
}

CancellationSource::CancellationSource():
    m_state(std::make_shared<detail::CancellationState>())
{
}

CancellationToken CancellationSource::Token() const noexcept
{
    return CancellationToken(m_state);
}

void CancellationSource::RequestCancel() noexcept
{
    m_state->Request();
}

} // namespace bbt::coroutine
