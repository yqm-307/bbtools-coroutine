/**
 * @file __TCoSelect.hpp
 * @brief CoSelect 的模板与 inline 成员实现，由 CoSelect.hpp 末尾包含。
 */
#pragma once
#include <algorithm>
#include <numeric>
#include <bbt/coroutine/sync/CoSelect.hpp>

namespace bbt::coroutine::sync {

template <class TItem, int Max> class CoSelect::ChanBranch final : public CoSelect::Branch {
    using Core = ChannelCore<TItem, Max>;
    using Attempt = typename Core::Attempt;
    using Mode = typename Core::Mode;
    using State = typename Core::State;

public:
    ChanBranch(
        const std::shared_ptr<Core>& core,
        const std::shared_ptr<SelectChoice>& choice,
        int index,
        TItem* out,
        const TItem* source)
        : m_core(core)
        , m_op(
              std::make_shared<typename Core::Operation>(
                  source != nullptr, m_value, SelectChoice::Clock::time_point::max()))
        , m_out(out)
        , m_source(source)
    {
        m_op->choice = choice;
        m_op->index = index;
    }

    ~ChanBranch() override
    {
        if (!m_core->Resolve(m_op, State::Unavailable) && m_out && m_core->ReadState(m_op) == State::Matched)
            m_core->Finish(m_op, State::Failed);
    }

    bool Probe(detail::Coroutine* co, bool immediate) override
    {
        const auto mode = immediate ? Mode::Immediate : Mode::Blocking;
        auto attempt = m_core->Admit(*m_op, m_op, co, mode, {});
        if (attempt == Attempt::Prepare) {
            m_value = std::make_unique<TItem>(*m_source);
            attempt = m_core->Admit(*m_op, m_op, co, mode, {});
        }
        return attempt == Attempt::Arm;
    }

    bool PairWithReceipt(detail::Coroutine* co, const Wait& wait) override
    {
        typename Core::Queue no_publication;
        return m_core->Admit(*m_op, m_op, co, Mode::Blocking, wait, &no_publication) == Attempt::Park;
    }

    void PrepareToPublish() override
    {
        if (m_source && !m_value && !m_core->IsClosed())
            m_value = std::make_unique<TItem>(*m_source);
        m_publication.push_back(m_op);
    }

    void Publish(detail::Coroutine* co, const Wait& wait) override
    {
        m_core->Admit(*m_op, m_op, co, Mode::Blocking, wait, &m_publication);
    }

    bool Complete(bool immediate) override
    {
        const auto state = m_core->ReadState(m_op);
        if (m_out && state == State::Matched) {
            m_core->DeliverMatched(m_op, [this]() { *m_out = std::as_const(*m_value); });
            return true;
        }
        if (immediate && (state == State::Matched || state == State::Failed))
            return true;
        return state == State::Closed || state == State::Completed;
    }

private:
    std::shared_ptr<Core> m_core;
    typename Core::Value m_value;
    typename Core::Op m_op;
    typename Core::Queue m_publication;
    TItem* m_out;
    const TItem* m_source;
};

template <class TItem, int Max> CoSelect& CoSelect::CaseRead(Chan<TItem, Max>& ch, TItem& out)
{
    m_cases.emplace_back([core = ch.m_core, &out](const std::shared_ptr<SelectChoice>& choice, int index) {
        return std::make_unique<ChanBranch<TItem, Max>>(core, choice, index, std::addressof(out), nullptr);
    });
    return *this;
}

template <class TItem, int Max> CoSelect& CoSelect::CaseRead(std::shared_ptr<Chan<TItem, Max>> ch, TItem& out)
{
    return CaseRead(*ch, out);
}

template <class TItem, int Max> CoSelect& CoSelect::CaseWrite(Chan<TItem, Max>& ch, const TItem& val)
{
    m_cases.emplace_back([core = ch.m_core, val](const std::shared_ptr<SelectChoice>& choice, int index) {
        return std::make_unique<ChanBranch<TItem, Max>>(core, choice, index, nullptr, std::addressof(val));
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

inline std::mt19937& CoSelect::_RandomEngine()
{
    static thread_local std::mt19937 engine{std::random_device{}()};
    return engine;
}

inline int CoSelect::Run()
{
    Assert(!m_cases.empty());
    auto* co = g_bbt_tls_helper->EnableUseCo() ? g_bbt_tls_coroutine_co : nullptr;
    if (co && co->IsCancelRequested())
        return -1;

    using Clock = SelectChoice::Clock;
    auto choice = std::make_shared<SelectChoice>();
    const auto interrupt = [weak = std::weak_ptr<SelectChoice>(choice)](short) {
        auto active = weak.lock();
        return active && active->Interrupt();
    };
    std::vector<size_t> order(m_cases.size());
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), _RandomEngine());

    std::vector<std::unique_ptr<Branch>> branches;
    branches.reserve(m_cases.size());
    for (size_t i = 0; i < m_cases.size(); ++i)
        branches.emplace_back(m_cases[i](choice, static_cast<int>(i)));

    int winner = SelectChoice::Pending;
    for (const auto index : order) {
        auto& branch = branches[index];
        if (branch->Probe(co, m_has_default) && co) {
            /**
             * 不先配对再登记回执：登记失败时接收方必须还未取得值。
             * 初次尝试不计入等待超时；一旦配对，只由回执结束本次等待。
             */
            if (!co->_WaitForOperation(Clock::time_point::max(), interrupt, [&](const Wait& wait) {
                    return branch->PairWithReceipt(co, wait);
                }))
                return -1;
        }
        winner = choice->Winner();
        if (winner != SelectChoice::Pending || (co && co->IsCancelRequested()))
            break;
    }

    if (winner == SelectChoice::Pending && co && !co->IsCancelRequested() && !m_has_default && m_timeout_ms != 0) {
        /**
         * 把会抛异常的节点分配和用户复制放在发布前；发布只移动所有权和预分配的链表节点。
         */
        for (const auto index : order)
            branches[index]->PrepareToPublish();
        const auto deadline =
            m_timeout_ms < 0 ? Clock::time_point::max() : Clock::now() + std::chrono::milliseconds(m_timeout_ms);
        choice->SetDeadline(deadline);
        try {
            if (!co->_WaitForOperation(deadline, interrupt, [&](const Wait& wait) {
                    for (const auto index : order) {
                        if (choice->Winner() != SelectChoice::Pending)
                            break;
                        branches[index]->Publish(co, wait);
                    }
                    return true;
                }))
                choice->Interrupt();
        } catch (...) {
            choice->Interrupt();
            throw;
        }
        winner = choice->Winner();
    }
    if (winner < 0)
        return -1;

    auto selected = std::move(branches[winner]);
    branches.clear();
    return selected->Complete(m_has_default) ? winner : -1;
}

} // namespace bbt::coroutine::sync
