#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <unistd.h>   // getpagesize()：校验保护页后栈底布局
#include <atomic>
#include <memory>

#include "bbt/coroutine/detail/Context.hpp"
#include "bbt/coroutine/detail/Stack.hpp"
#include "bbt/coroutine/detail/StackPool.hpp"

namespace {

/* #379 析构顺序探针：在析构期读取栈池借出计数，供测试体在 delete 完成后断言。
 * 析构中不抛异常、不做 Boost 断言——析构路径抛出会终止进程，断言在析构中也不
 * 可靠；这里只把观测值存到外部变量，判定留在测试体。 */
std::atomic_int g_probe_destroy_count{0};
std::atomic_int g_probe_cur_at_destroy{-1};

struct StackBorrowProbe
{
    ~StackBorrowProbe()
    {
        g_probe_cur_at_destroy.store(
            g_bbt_stackpoll->GetCurCoNum(), std::memory_order_relaxed);
        g_probe_destroy_count.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Coroutine_Stack)

BOOST_AUTO_TEST_CASE(t_coroutine_stack_create)
{
    auto single_stack = bbt::coroutine::detail::Stack(1024 * 4);

    std::vector<bbt::coroutine::detail::Stack> array;

    for (int i = 0; i < 1000; ++i)
    {
        array.push_back(bbt::coroutine::detail::Stack(1024*40));
    }
}

// #279：NDEBUG 下 assert(Free(...)==0) 把释放整个编译掉 → Release 构建栈内存泄漏。
// 契约：Clear() 后栈资源必须真正释放且对象回到空态（MemChunkBegin()==nullptr），
// 重复 Clear() 幂等、不二次 free。
BOOST_AUTO_TEST_CASE(t_stack_clear_releases_and_is_idempotent)
{
    bbt::coroutine::detail::Stack stk(1024 * 64, false);
    BOOST_REQUIRE(stk.MemChunkBegin() != nullptr);
    BOOST_REQUIRE(stk.MemChunkSize() > 0);

    stk.Clear();
    BOOST_CHECK(stk.MemChunkBegin() == nullptr);
    BOOST_CHECK(stk.StackTop() == nullptr);

    // 幂等：不得二次 free 同一指针（glibc 会 abort / 破坏堆）
    stk.Clear();
    BOOST_CHECK(stk.MemChunkBegin() == nullptr);
}

// RSS 探针在 stack_protect=false 时无效：memalign 的栈内存从不被写入，只是虚拟
// 预留、不驻留 RSS，free 与否都测不出增长；真要 touch 就会占满内存。故用上面
// idempotent 探针锁 bug——不真正 free 就不可能安全地把 m_mem_chunk 置 nullptr。

// #379：栈所有权必须能被安全转移。move 后源对象回空态，目标接管内存；
// 此时 Valgrind 栈登记（NEED_VALGRIND=ON 时）也必须随所有权转移，
// 否则源对象析构会重复注销、目标析构会漏注销。这里锁住对象层不变量。
BOOST_AUTO_TEST_CASE(t_stack_move_transfers_ownership)
{
    bbt::coroutine::detail::Stack a(1024 * 64);
    char*  const p  = a.MemChunkBegin();
    size_t const sz = a.MemChunkSize();
    BOOST_REQUIRE(p != nullptr);
    BOOST_REQUIRE(sz > 0);

    bbt::coroutine::detail::Stack b(std::move(a));
    BOOST_CHECK(a.MemChunkBegin() == nullptr);
    BOOST_CHECK(a.MemChunkSize() == 0);
    BOOST_CHECK(a.UseableSize() == 0);
    BOOST_CHECK(b.MemChunkBegin() == p);
    BOOST_CHECK(b.MemChunkSize() == sz);

    // 接管方释放后回空态，且重复 Clear 幂等（不得二次 free）
    b.Clear();
    BOOST_CHECK(b.MemChunkBegin() == nullptr);
    b.Clear();
    BOOST_CHECK(b.MemChunkBegin() == nullptr);
}

// #379：开启保护页时栈底必须落在保护页之后（保护页保持 PROT_NONE，不被登记成栈）；
// Clear 后所有可见状态归零，地址可被后续分配真实复用（不是泄漏后残留）。
BOOST_AUTO_TEST_CASE(t_stack_protect_layout_and_clear_resets_state)
{
    bbt::coroutine::detail::Stack s(1024 * 64, true);
    BOOST_REQUIRE(s.MemChunkBegin() != nullptr);
    BOOST_REQUIRE(s.UseableSize() > 0);
    BOOST_CHECK_EQUAL(s.StackBottom(),
                      s.MemChunkBegin() + static_cast<size_t>(getpagesize()));

    s.Clear();
    BOOST_CHECK(s.MemChunkBegin() == nullptr);
    BOOST_CHECK(s.StackTop() == nullptr);
    BOOST_CHECK(s.StackBottom() == nullptr);
    BOOST_CHECK(s.UseableSize() == 0);

    // 真实归还后重新申请应成功（复用路径）
    bbt::coroutine::detail::Stack t(1024 * 64, true);
    BOOST_CHECK(t.MemChunkBegin() != nullptr);
    t.Clear();
    BOOST_CHECK(t.MemChunkBegin() == nullptr);
}

// #379：回调释放必须先于栈归还。原 ~Context 只 Release(m_stack)，改由成员逆序
// 析构 callable 之后才释放捕获对象——对象析构落在“栈已归还”之后。本用例直接
// 构造 Context（不启动 Scheduler），令 m_user_main 内的 callable 成为探针
// shared_ptr 的唯一持有者；探针析构时读取栈池借出计数，断言析构发生在归还之前
// （cur == baseline + 1），且 delete 完成后计数回到 baseline。
// 覆盖边界：只覆盖 m_user_main 这条赋值路径。m_onyield_callback 仅在
// YieldWithCallback 挂起期间非空，正常 FINAL 路径已由协程侧清空；不引入 test
// seam 就无法确定性地构造其保留态，故本轮不覆盖。
BOOST_AUTO_TEST_CASE(t_context_releases_callables_before_stack_return)
{
    using bbt::coroutine::detail::Context;
    using bbt::coroutine::detail::CoroutineCallback;

    const int baseline = g_bbt_stackpoll->GetCurCoNum();

    g_probe_destroy_count.store(0, std::memory_order_relaxed);
    g_probe_cur_at_destroy.store(-1, std::memory_order_relaxed);

    /* 显式 new 构造唯一共享对象（make_shared 亦只析构一次，但显式 new 语义最直白，
     * 不会因聚合临时量造成额外计数）。 */
    std::shared_ptr<StackBorrowProbe> owner{new StackBorrowProbe{}};

    /* 绑定 std::function 临时量：Context 构造仅从它拷贝一次到 m_user_main；临时量
     * 在本语句结束时销毁，随后清空外部 owner，令 m_user_main 成为唯一持有者。 */
    Context* ctx = new Context(
        1024 * 64, CoroutineCallback{[owner]() { (void)owner; }}, false);
    owner.reset();

    delete ctx;

    BOOST_CHECK_EQUAL(g_probe_destroy_count.load(std::memory_order_relaxed), 1);
    BOOST_CHECK_EQUAL(g_probe_cur_at_destroy.load(std::memory_order_relaxed),
                      baseline + 1);
    BOOST_CHECK_EQUAL(g_bbt_stackpoll->GetCurCoNum(), baseline);
}

// #379：memcheck 前必须释放栈池中为复用保留的闲置栈；这不是 Scheduler 停机，
// 只在所有借出栈已归还后清理池内缓存。
BOOST_AUTO_TEST_CASE(t_stack_pool_releases_unused_stacks)
{
    bbt::coroutine::detail::StackPool pool;
    auto* stack = pool.Apply();
    BOOST_REQUIRE(stack != nullptr);
    pool.Release(stack);
    BOOST_REQUIRE(pool.AllocSize() > 0);

    const size_t released = pool.ReleaseUnused();

    BOOST_CHECK(released > 0);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 0);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
