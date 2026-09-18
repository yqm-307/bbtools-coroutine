/**
 * @file Test_cancellation_token.cc
 * @brief #347 C1 CancellationToken/CancellationSource 验收：
 *        默认 token 永不取消、RequestCancel 幂等（多次/多线程）、
 *        Combine（父取消/子取消/两者都取消/默认 token 参与/已取消输入）、
 *        复制后语义一致、组合释放后源取消安全、OR 视图嵌套扁平化
 *        （临时中间 token 不保留仍传播）、等待登记覆盖全部源与整组撤销。
 *
 * 契约：build-design-contract/agent-docs/2026-09-17-service-runtime-contract-v1.md §C1
 * 本类型不依赖 Scheduler，全部用例为纯值语义 + 多线程竞争。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/sync/Cancellation.hpp>

namespace bbt::coroutine
{

/* 单测探针：触达令牌的等待订阅入口（仓内 CoRWMutexTestAccess 同款模式）。
 * _RegisterCancelCallback/_UnregisterCancelCallback 本是等待路径
 * （CompletionSignal::Wait）专用，这里借探针做不依赖 Scheduler 的
 * 纯值语义验证。 */
class CancellationTokenTestAccess
{
public:
    static std::uint64_t Register(const CancellationToken& token, std::function<void()> cb)
    {
        return token._RegisterCancelCallback(std::move(cb));
    }

    static void Unregister(const CancellationToken& token, std::uint64_t id) noexcept
    {
        token._UnregisterCancelCallback(id);
    }
};

} // namespace bbt::coroutine

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(CancellationTokenTest)

/* 默认构造的 token 永不取消；复制不引入状态 */
BOOST_AUTO_TEST_CASE(t_default_token_never_cancels)
{
    CancellationToken token;
    BOOST_CHECK(!token.IsCancellationRequested());

    CancellationToken copy = token;
    BOOST_CHECK(!copy.IsCancellationRequested());

    /* 两个默认 token 组合仍是永不取消 */
    auto combined = CancellationToken::Combine(token, copy);
    BOOST_CHECK(!combined.IsCancellationRequested());
}

/* RequestCancel 幂等：多次调用、取消后状态保持 */
BOOST_AUTO_TEST_CASE(t_source_request_idempotent)
{
    CancellationSource source;
    auto token = source.Token();
    BOOST_CHECK(!token.IsCancellationRequested());

    source.RequestCancel();
    BOOST_CHECK(token.IsCancellationRequested());

    /* 重复调用不报错、不改变已取消状态 */
    source.RequestCancel();
    source.RequestCancel();
    BOOST_CHECK(token.IsCancellationRequested());
}

/* 多线程并发 RequestCancel 幂等、线程安全 */
BOOST_AUTO_TEST_CASE(t_source_request_multithreaded)
{
    CancellationSource source;
    auto token = source.Token();
    constexpr int kThreads = 8;

    bbt::core::thread::CountDownLatch go{1};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back([&]() {
            go.Wait();
            source.RequestCancel();
        });

    go.Down();
    for (auto& t : threads)
        t.join();

    BOOST_CHECK(token.IsCancellationRequested());
}

/* 复制后的 token 与源共享状态：取消前后观察一致 */
BOOST_AUTO_TEST_CASE(t_copy_shares_state)
{
    CancellationSource source;
    auto token = source.Token();
    auto copy1 = token;
    auto copy2 = source.Token();

    BOOST_CHECK(!copy1.IsCancellationRequested());
    BOOST_CHECK(!copy2.IsCancellationRequested());

    source.RequestCancel();
    BOOST_CHECK(token.IsCancellationRequested());
    BOOST_CHECK(copy1.IsCancellationRequested());
    BOOST_CHECK(copy2.IsCancellationRequested());
}

/* Combine：父侧取消即取消 */
BOOST_AUTO_TEST_CASE(t_combine_parent_cancels)
{
    CancellationSource parent_src, extra_src;
    auto combined = CancellationToken::Combine(parent_src.Token(), extra_src.Token());

    BOOST_CHECK(!combined.IsCancellationRequested());
    parent_src.RequestCancel();
    BOOST_CHECK(combined.IsCancellationRequested());
    BOOST_CHECK(!extra_src.Token().IsCancellationRequested());
}

/* Combine：子侧（extra）取消即取消 */
BOOST_AUTO_TEST_CASE(t_combine_extra_cancels)
{
    CancellationSource parent_src, extra_src;
    auto combined = CancellationToken::Combine(parent_src.Token(), extra_src.Token());

    extra_src.RequestCancel();
    BOOST_CHECK(combined.IsCancellationRequested());
}

/* Combine：两者都取消仍为取消（传播不重复报错） */
BOOST_AUTO_TEST_CASE(t_combine_both_cancel)
{
    CancellationSource parent_src, extra_src;
    auto combined = CancellationToken::Combine(parent_src.Token(), extra_src.Token());

    parent_src.RequestCancel();
    extra_src.RequestCancel();
    BOOST_CHECK(combined.IsCancellationRequested());
}

/* Combine：任一输入在组合前已取消，组合立即取消 */
BOOST_AUTO_TEST_CASE(t_combine_already_cancelled_input)
{
    CancellationSource cancelled_src, live_src;
    cancelled_src.RequestCancel();

    auto c1 = CancellationToken::Combine(cancelled_src.Token(), live_src.Token());
    BOOST_CHECK(c1.IsCancellationRequested());

    auto c2 = CancellationToken::Combine(live_src.Token(), cancelled_src.Token());
    BOOST_CHECK(c2.IsCancellationRequested());
}

/* Combine：默认 token 参与退化为另一侧语义 */
BOOST_AUTO_TEST_CASE(t_combine_with_default_token)
{
    CancellationSource source;
    CancellationToken never;

    auto c1 = CancellationToken::Combine(source.Token(), never);
    auto c2 = CancellationToken::Combine(never, source.Token());
    BOOST_CHECK(!c1.IsCancellationRequested());
    BOOST_CHECK(!c2.IsCancellationRequested());

    source.RequestCancel();
    BOOST_CHECK(c1.IsCancellationRequested());
    BOOST_CHECK(c2.IsCancellationRequested());
}

/* Combine 的组合 token 可复制且语义一致 */
BOOST_AUTO_TEST_CASE(t_combine_copy_consistent)
{
    CancellationSource parent_src, extra_src;
    auto combined = CancellationToken::Combine(parent_src.Token(), extra_src.Token());
    auto copy = combined;

    extra_src.RequestCancel();
    BOOST_CHECK(combined.IsCancellationRequested());
    BOOST_CHECK(copy.IsCancellationRequested());
}

/* 组合释放后源取消安全：OR 视图下 Combine 不在源上登记任何回调，
 * 组合析构与源完全无耦合，源取消不得访问已消亡的组合 */
BOOST_AUTO_TEST_CASE(t_combine_release_unbinds)
{
    CancellationSource parent_src, extra_src;
    {
        auto combined = CancellationToken::Combine(parent_src.Token(), extra_src.Token());
        BOOST_CHECK(!combined.IsCancellationRequested());
    }
    /* combined 已析构：源取消不触及组合残留，不 crash */
    parent_src.RequestCancel();
    extra_src.RequestCancel();
    BOOST_CHECK(parent_src.Token().IsCancellationRequested());
    BOOST_CHECK(extra_src.Token().IsCancellationRequested());
}

/* 组合可继续参与组合（链式传播） */
BOOST_AUTO_TEST_CASE(t_combine_chain_propagates)
{
    CancellationSource a, b, c;
    auto ab = CancellationToken::Combine(a.Token(), b.Token());
    auto abc = CancellationToken::Combine(ab, c.Token());

    c.RequestCancel();
    BOOST_CHECK(abc.IsCancellationRequested());
    BOOST_CHECK(!ab.IsCancellationRequested());

    CancellationSource a2, b2, c2;
    auto ab2 = CancellationToken::Combine(a2.Token(), b2.Token());
    auto abc2 = CancellationToken::Combine(ab2, c2.Token());
    a2.RequestCancel();
    BOOST_CHECK(ab2.IsCancellationRequested());
    BOOST_CHECK(abc2.IsCancellationRequested());
}

/* OR 视图回归（旧实现的 BROKEN 场景）：嵌套组合的中间 token 是临时
 * 对象、不保留在变量里，任一源取消仍须传播到外层组合。
 * 旧实现靠组合状态间 weak_ptr 传播：临时组合析构即断链，a 的取消
 * 到不了 nested —— 本条断言在旧实现下必失败。 */
BOOST_AUTO_TEST_CASE(t_combine_nested_temp_not_kept)
{
    CancellationSource a, b, c;
    auto nested = CancellationToken::Combine(
        CancellationToken::Combine(a.Token(), b.Token()),
        c.Token());

    BOOST_CHECK(!nested.IsCancellationRequested());
    a.RequestCancel();
    BOOST_CHECK(nested.IsCancellationRequested());
}

/* 对照组：中间 token 保留在变量里时链式语义同样成立 */
BOOST_AUTO_TEST_CASE(t_combine_nested_temp_kept)
{
    CancellationSource a, b, c;
    auto mid = CancellationToken::Combine(a.Token(), b.Token());
    auto chained = CancellationToken::Combine(mid, c.Token());

    BOOST_CHECK(!chained.IsCancellationRequested());
    a.RequestCancel();
    BOOST_CHECK(mid.IsCancellationRequested());
    BOOST_CHECK(chained.IsCancellationRequested());
}

/* 三源组合的 OR 语义：逐源独立验证任一取消即整体取消 */
BOOST_AUTO_TEST_CASE(t_combine_three_sources_any_cancels)
{
    for (int which = 0; which < 3; ++which) {
        CancellationSource srcs[3];
        auto combined = CancellationToken::Combine(
            CancellationToken::Combine(srcs[0].Token(), srcs[1].Token()),
            srcs[2].Token());

        BOOST_CHECK(!combined.IsCancellationRequested());
        srcs[which].RequestCancel();
        BOOST_CHECK_MESSAGE(combined.IsCancellationRequested(),
            "取消第 " << which << " 个源后组合未取消");
    }
}

/* 等待订阅登记到每个源：任一源取消都触发回调；全部取消允许重复
 * 通知（每源各派一次），但至少要被通知一次。含已取消源分支：
 * Register 对已取消状态同步回调并返回 0，不计入登记簿。 */
BOOST_AUTO_TEST_CASE(t_combine_registration_reaches_all_sources)
{
    {
        CancellationSource a, b, c;
        auto combined = CancellationToken::Combine(
            CancellationToken::Combine(a.Token(), b.Token()),
            c.Token());

        std::atomic_int notified{0};
        const auto group = CancellationTokenTestAccess::Register(
            combined, [&notified]() { notified.fetch_add(1); });
        BOOST_REQUIRE_NE(group, 0u);

        /* 各源上的登记相互独立：取消第 N 个源计数必须继续增长，
         * 若某源漏登记，其取消不会带来新增通知 */
        int last = 0;
        a.RequestCancel();
        BOOST_CHECK_GT(notified.load(), last);
        last = notified.load();
        b.RequestCancel();
        BOOST_CHECK_GT(notified.load(), last);
        last = notified.load();
        c.RequestCancel();
        BOOST_CHECK_GT(notified.load(), last);

        CancellationTokenTestAccess::Unregister(combined, group);
    }

    /* 组合时已有源取消：该源同步派发回调，其余源正常登记可撤销 */
    {
        CancellationSource dead, live;
        dead.RequestCancel();
        auto combined = CancellationToken::Combine(dead.Token(), live.Token());

        std::atomic_int notified{0};
        const auto group = CancellationTokenTestAccess::Register(
            combined, [&notified]() { notified.fetch_add(1); });
        BOOST_CHECK_GE(notified.load(), 1);
        CancellationTokenTestAccess::Unregister(combined, group);
    }

    /* 全部源已取消：无登记可建，返回 0，回调已同步派发 */
    {
        CancellationSource dead1, dead2;
        dead1.RequestCancel();
        dead2.RequestCancel();
        auto combined = CancellationToken::Combine(dead1.Token(), dead2.Token());

        std::atomic_int notified{0};
        const auto group = CancellationTokenTestAccess::Register(
            combined, [&notified]() { notified.fetch_add(1); });
        BOOST_CHECK_EQUAL(group, 0u);
        BOOST_CHECK_GE(notified.load(), 1);
    }
}

/* 撤销登记覆盖全部源：Unregister 后任一源再取消都不再触发回调 */
BOOST_AUTO_TEST_CASE(t_combine_unregister_covers_all)
{
    CancellationSource a, b, c;
    auto combined = CancellationToken::Combine(
        CancellationToken::Combine(a.Token(), b.Token()),
        c.Token());

    std::atomic_int notified{0};
    const auto group = CancellationTokenTestAccess::Register(
        combined, [&notified]() { notified.fetch_add(1); });
    BOOST_REQUIRE_NE(group, 0u);
    CancellationTokenTestAccess::Unregister(combined, group);

    a.RequestCancel();
    b.RequestCancel();
    c.RequestCancel();
    BOOST_CHECK_EQUAL(notified.load(), 0);
    BOOST_CHECK(combined.IsCancellationRequested());
}

/* 默认 token 退化语义：两个默认组合仍永不取消；
 * 登记返回 0 且不产生任何回调（等待侧无登记可撤销） */
BOOST_AUTO_TEST_CASE(t_combine_of_default_tokens)
{
    CancellationToken never1, never2;
    auto combined = CancellationToken::Combine(never1, never2);

    BOOST_CHECK(!combined.IsCancellationRequested());

    std::atomic_int notified{0};
    const auto group = CancellationTokenTestAccess::Register(
        combined, [&notified]() { notified.fetch_add(1); });
    BOOST_CHECK_EQUAL(group, 0u);
    BOOST_CHECK_EQUAL(notified.load(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
