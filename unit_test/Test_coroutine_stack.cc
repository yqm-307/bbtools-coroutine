#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <unistd.h>       // getpagesize() / fork()：校验保护页布局与隔离故障子进程
#include <signal.h>       // SIGSEGV
#include <sys/wait.h>     // waitpid：父进程核验子进程真实终止状态

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "bbt/coroutine/detail/Context.hpp"
#include "bbt/coroutine/detail/Coroutine.hpp"
#include "bbt/coroutine/detail/Define.hpp"
#include "bbt/coroutine/detail/GlobalConfig.hpp"
#include "bbt/coroutine/detail/Scheduler.hpp"
#include "bbt/coroutine/detail/Stack.hpp"
#include "bbt/coroutine/detail/StackPool.hpp"

namespace {

/* #379 析构顺序探针：在析构期读取栈池借出计数，供测试体在 delete 完成后断言。
 * 析构中不抛异常、不做 Boost 断言——析构路径抛出会终止进程，断言在析构中也不
 * 可靠；这里只把观测值存到外部变量，判定留在测试体。
 * 计数口径已改为 size_t（借出 + 申请中），探针类型同步，不做窄化。 */
std::atomic_int    g_probe_destroy_count{0};
std::atomic<size_t> g_probe_cur_at_destroy{static_cast<size_t>(-1)};

struct StackBorrowProbe
{
    ~StackBorrowProbe()
    {
        g_probe_cur_at_destroy.store(
            g_bbt_stackpoll->GetCurCoNum(), std::memory_order_relaxed);
        g_probe_destroy_count.fetch_add(1, std::memory_order_relaxed);
    }
};

/* 只在没有 runtime、没有其他线程时改全局配置，并在作用域结束时还原
 * （设计 §风险与恢复：不在已启动进程里重启 runtime 来重置配置）。 */
struct ConfigGuard
{
    size_t max_coroutine;
    size_t static_coroutine;
    size_t stack_size;
    bool   stack_protect;
    size_t stackpool_min;

    ConfigGuard()
    {
        auto* cfg = g_bbt_coroutine_config.get();
        max_coroutine    = cfg->m_cfg_max_coroutine;
        static_coroutine = cfg->m_cfg_static_coroutine;
        stack_size       = cfg->m_cfg_stack_size;
        stack_protect    = cfg->m_cfg_stack_protect;
        stackpool_min    = cfg->m_cfg_stackpool_min_alloc_size;
    }

    ~ConfigGuard()
    {
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_max_coroutine              = max_coroutine;
        cfg->m_cfg_static_coroutine           = static_coroutine;
        cfg->m_cfg_stack_size                 = stack_size;
        cfg->m_cfg_stack_protect              = stack_protect;
        cfg->m_cfg_stackpool_min_alloc_size   = stackpool_min;
    }
};

/* 复用式线程屏障：S7 用它固定「同时申请」与「成功者持有到第二屏障」两个时刻，
 * 不靠任意 sleep 排序；带超时，异常路径不会把测试挂死。 */
class Barrier
{
public:
    explicit Barrier(size_t total): m_total(total) {}

    bool Wait(int timeout_ms = 30000)
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        const size_t generation = m_generation;
        if (++m_arrived == m_total) {
            m_arrived = 0;
            ++m_generation;
            m_cv.notify_all();
            return true;
        }
        return m_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                             [&]{ return generation != m_generation; });
    }

private:
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    const size_t            m_total;
    size_t                  m_arrived{0};
    size_t                  m_generation{0};
};

} // namespace

BOOST_AUTO_TEST_SUITE(Coroutine_Stack)

// 显式大小必须贯通 Context→StackPool，不能仍借出全局默认尺寸。
BOOST_AUTO_TEST_CASE(t_context_honors_explicit_stack_size)
{
    const size_t page = static_cast<size_t>(getpagesize());
    const size_t request = 64 * 1024 + 1;
    const size_t expected = request + (page - request % page) % page;
    bbt::coroutine::detail::Context large(request, []() {}, false);
    bbt::coroutine::detail::Context medium(32 * 1024, []() {}, false);
    BOOST_CHECK_EQUAL(large.GetStackSize(), expected);
    BOOST_CHECK_EQUAL(medium.GetStackSize(), 32 * 1024);
}

// S1（后半）：不启动 runtime 的出厂默认路径。省略大小只取全局默认（出厂 12 KiB），
// 显式调用既不改写默认，也不污染默认桶。
BOOST_AUTO_TEST_CASE(t_factory_default_stack_size_without_runtime)
{
    using bbt::coroutine::detail::StackPool;

    ConfigGuard guard;
    auto* cfg = g_bbt_coroutine_config.get();
    BOOST_REQUIRE_EQUAL(cfg->m_cfg_stack_size, 1024u * 12);    // 出厂默认；本二进制内无人改写
    BOOST_REQUIRE_EQUAL(cfg->m_cfg_stack_protect, true);

    StackPool pool;

    auto* by_default = pool.Apply();            // 无参入口 = 默认配置
    BOOST_REQUIRE(by_default != nullptr);
    BOOST_CHECK_EQUAL(by_default->UseableSize(), 12288u);
    BOOST_CHECK(by_default->IsProtected());

    const size_t explicit_request = 20 * 1024;
    auto* by_explicit = pool.Apply(explicit_request, true);
    BOOST_REQUIRE(by_explicit != nullptr);
    BOOST_CHECK_EQUAL(by_explicit->UseableSize(), explicit_request);

    /* 显式调用不改写全局默认，也不改保护开关 */
    BOOST_CHECK_EQUAL(cfg->m_cfg_stack_size, 1024u * 12);
    BOOST_CHECK_EQUAL(cfg->m_cfg_stack_protect, true);

    pool.Release(by_default);
    pool.Release(by_explicit);

    auto* default_again = pool.Apply();
    BOOST_CHECK(default_again == by_default);   // 默认桶按默认配置回桶，未被显式调用污染
    pool.Release(default_again);

    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);
    BOOST_CHECK(pool.ReleaseUnused() >= 2);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 0u);
}

BOOST_AUTO_TEST_CASE(t_coroutine_stack_create)
{
    auto single_stack = bbt::coroutine::detail::Stack(1024 * 4);

    std::vector<bbt::coroutine::detail::Stack> array;

    for (int i = 0; i < 1000; ++i)
    {
        array.push_back(bbt::coroutine::detail::Stack(1024*40));
    }
}

// S2：页对齐与保护布局。小值（1 / P-1 / P / P+1 / 12 KiB / 16 KiB+1 / 64 KiB）
// 只测内存布局，不在极小栈上执行测试框架。
BOOST_AUTO_TEST_CASE(t_stack_size_alignment_and_guard_layout)
{
    using bbt::coroutine::detail::Stack;
    const size_t P = static_cast<size_t>(getpagesize());
    const size_t requests[] = {1, P - 1, P, P + 1, 12 * 1024, 16 * 1024 + 1, 64 * 1024};

    for (size_t req : requests)
    {
        Stack protected_stack(req, true);
        const size_t U = protected_stack.UseableSize();
        BOOST_CHECK(U >= req);
        BOOST_CHECK_EQUAL(U % P, 0u);
        BOOST_CHECK(U - req < P);
        BOOST_CHECK_EQUAL(
            reinterpret_cast<uintptr_t>(protected_stack.MemChunkBegin()) % P, 0u);
        BOOST_CHECK_EQUAL(protected_stack.MemChunkSize(), U + P);
        BOOST_CHECK(protected_stack.IsProtected());
        BOOST_CHECK_EQUAL(protected_stack.StackBottom(),
                          protected_stack.MemChunkBegin() + P);

        /* 正常可用区末端可写；保护页不属于登记的可用区（另行子进程验证不可读） */
        protected_stack.StackTop()[-1] = static_cast<char>(0x5a);
        BOOST_CHECK_EQUAL(
            static_cast<unsigned char>(protected_stack.StackTop()[-1]), 0x5au);

        Stack unprotected_stack(req, false);
        BOOST_CHECK_EQUAL(unprotected_stack.UseableSize(), Stack::NormalizeSize(req));
        BOOST_CHECK(!unprotected_stack.IsProtected());
        BOOST_CHECK_EQUAL(unprotected_stack.MemChunkSize(), unprotected_stack.UseableSize());
        BOOST_CHECK_EQUAL(unprotected_stack.StackBottom(), unprotected_stack.MemChunkBegin());
        BOOST_CHECK_EQUAL(
            reinterpret_cast<uintptr_t>(unprotected_stack.MemChunkBegin()) % P, 0u);
    }
}

// S3：保护页/无保护栈首地址的隔离故障子进程。父进程检查 wait 状态，不看属性字段。
BOOST_AUTO_TEST_CASE(t_stack_guard_page_subprocess_sigsegv)
{
    const size_t P = static_cast<size_t>(getpagesize());

    /* 保护栈：读保护页必须让子进程收到 SIGSEGV */
    std::fflush(nullptr);       // 子进程继承空缓冲：避免重复输出测试头
    pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0)
    {
        /* Boost.Test 的 execution_monitor 默认接管 SIGSEGV 并把它转成「测试失败」而不是
         * 真实信号终止；子进程里先恢复默认处置，父进程才能核到真正的 WIFSIGNALED。 */
        ::signal(SIGSEGV, SIG_DFL);
        ::signal(SIGBUS, SIG_DFL);
        ::signal(SIGABRT, SIG_DFL);
        bbt::coroutine::detail::Stack stack(16 * 1024, true);
        volatile char* guard = stack.MemChunkBegin() + P - 1;   // 保护页最后一字节
        *guard = 1;
        ::_exit(0);     // 不该到达：保护页可写就说明保护没生效
    }
    int status = 0;
    BOOST_REQUIRE_EQUAL(::waitpid(pid, &status, 0), pid);
    BOOST_CHECK(WIFSIGNALED(status));
    BOOST_CHECK_EQUAL(WTERMSIG(status), SIGSEGV);

    /* 正对照：无保护栈的对应首地址可写，子进程正常退出 */
    std::fflush(nullptr);
    pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0)
    {
        ::signal(SIGSEGV, SIG_DFL);
        bbt::coroutine::detail::Stack stack(16 * 1024, false);
        volatile char* first = stack.MemChunkBegin();
        *first = 1;
        for (size_t i = 0; i < stack.UseableSize(); i += P)
            stack.MemChunkBegin()[i] = 1;
        ::_exit(*first == 1 ? 0 : 2);
    }
    status = 0;
    BOOST_REQUIRE_EQUAL(::waitpid(pid, &status, 0), pid);
    BOOST_CHECK(WIFEXITED(status));
    BOOST_CHECK_EQUAL(WEXITSTATUS(status), 0);
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

// S13：解除保护失败属于「不可安全 free」的系统故障，必须在隔离子进程中观察到
// fail-stop（terminate），并区分「命中 terminate」「普通返回」「无关崩溃」；
// 不能把任意子进程失败算通过。runtime/其他线程未启动时才 fork。
BOOST_AUTO_TEST_CASE(t_release_protect_failure_is_fail_stop)
{
    const int kTerminateSentinel = 42;
    const int kNormalExitCode    = 7;

    std::fflush(nullptr);
    pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0)
    {
        std::set_terminate([]() { ::_exit(kTerminateSentinel); });
        bbt::coroutine::detail::Stack::_TestFailNextReleaseProtect();
        {
            bbt::coroutine::detail::Stack stack(16 * 1024, true);
            stack.Clear();          // 解除保护失败 → 不得继续 free → terminate
        }
        ::_exit(0);                 // 普通返回：说明被当成可恢复成功，判失败
    }
    int status = 0;
    BOOST_REQUIRE_EQUAL(::waitpid(pid, &status, 0), pid);
    BOOST_CHECK(WIFEXITED(status));
    BOOST_CHECK_EQUAL(WEXITSTATUS(status), kTerminateSentinel);

    /* 正对照：未注入时正常返回并真正释放，不得被误判为 fail-stop */
    std::fflush(nullptr);
    pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0)
    {
        std::set_terminate([]() { ::_exit(kTerminateSentinel); });
        bbt::coroutine::detail::Stack stack(16 * 1024, true);
        stack.Clear();
        const bool cleared = (stack.MemChunkBegin() == nullptr) && (stack.UseableSize() == 0);
        ::_exit(cleared ? kNormalExitCode : 9);
    }
    status = 0;
    BOOST_REQUIRE_EQUAL(::waitpid(pid, &status, 0), pid);
    BOOST_CHECK(WIFEXITED(status));
    BOOST_CHECK_EQUAL(WEXITSTATUS(status), kNormalExitCode);
}

// S9：非法值必须在分配/指针运算之前被拒绝，异常类别符合契约，且不改动池计数。
BOOST_AUTO_TEST_CASE(t_invalid_stack_size_rejected_before_alloc)
{
    using bbt::coroutine::detail::Context;
    using bbt::coroutine::detail::Coroutine;
    using bbt::coroutine::detail::CoroutineCallback;
    using bbt::coroutine::detail::Stack;
    using bbt::coroutine::detail::StackPool;

    const size_t pool_alloc = g_bbt_stackpoll->AllocSize();
    const size_t pool_cur   = g_bbt_stackpoll->GetCurCoNum();
    CoroutineCallback empty = [](){};

    /* 0 与 resize 溢出类别 */
    BOOST_CHECK_THROW(Stack(0, true), std::invalid_argument);
    BOOST_CHECK_THROW(Stack(0, false), std::invalid_argument);
    /* 旧的 signed 调用转换后（-1）也必须被范围检查拒绝，不静默变小 */
    BOOST_CHECK_THROW(Stack(static_cast<size_t>(-1), false), std::length_error);
    BOOST_CHECK_THROW(
        Stack(static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max()) - 1, false),
        std::length_error);
    /* 页取整加法回绕 */
    BOOST_CHECK_THROW(Stack(std::numeric_limits<size_t>::max(), false), std::length_error);
    BOOST_CHECK_THROW(
        Stack(std::numeric_limits<size_t>::max() - 1, true), std::length_error);

    /* 创建/显式注册入口同样在分配前拒绝 */
    BOOST_CHECK_THROW(Coroutine::Create(0, empty), std::invalid_argument);
    BOOST_CHECK_THROW(Coroutine::Create(static_cast<size_t>(-1), empty), std::length_error);
    {
        StackPool pool;
        BOOST_CHECK_THROW(pool.Apply(0, true), std::invalid_argument);
        BOOST_CHECK_THROW(pool.Apply(std::numeric_limits<size_t>::max(), false), std::length_error);
    }

    BOOST_CHECK_EQUAL(g_bbt_stackpoll->AllocSize(), pool_alloc);
    BOOST_CHECK_EQUAL(g_bbt_stackpoll->GetCurCoNum(), pool_cur);

    /* Context 入口：0 也在借栈前被拒绝 */
    BOOST_CHECK_THROW(Context(0, [](){}, false), std::invalid_argument);
    BOOST_CHECK_EQUAL(g_bbt_stackpoll->AllocSize(), pool_alloc);
}

// S9/R6：低层池容量不足返回空；创建/注册抛 runtime_error；成功输出重载不抛并置 false。
BOOST_AUTO_TEST_CASE(t_pool_capacity_exhaustion_contract)
{
    using bbt::coroutine::detail::Context;
    using bbt::coroutine::detail::StackPool;

    ConfigGuard guard;
    auto* cfg = g_bbt_coroutine_config.get();
    cfg->m_cfg_max_coroutine = 0;   // m_cfg_stackpool_max_alloc_size 是它的引用

    StackPool pool;
    BOOST_CHECK(pool.Apply(4096, false) == nullptr);
    BOOST_CHECK_THROW(Context(4096, [](){}, false), std::runtime_error);

    /* 无 worker 的注册拒绝：成功输出重载不抛、置 false，且不留借出对象 */
    bool succ = true;
    g_scheduler->RegistCoroutineTask([](){}, succ);
    BOOST_CHECK(!succ);
    succ = true;
    g_scheduler->RegistCoroutineTask(64 * 1024, [](){}, succ);
    BOOST_CHECK(!succ);
    BOOST_CHECK_EQUAL(g_bbt_stackpoll->GetCurCoNum(), 0u);

    cfg->m_cfg_max_coroutine = guard.max_coroutine;
    auto* restored = pool.Apply(4096, false);
    BOOST_REQUIRE(restored != nullptr);
    pool.Release(restored);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);
    BOOST_CHECK_EQUAL(pool.ReleaseUnused(), 1u);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 0u);
}

// S5/S6：精确按 (U, protect) 分桶复用；相同归一化大小同桶；较大桶不替代较小桶；
// 归还只按栈自身属性，不读全局默认值。
BOOST_AUTO_TEST_CASE(t_pool_reuses_exact_bucket_and_isolates_attributes)
{
    using bbt::coroutine::detail::StackPool;

    ConfigGuard guard;
    const size_t P = static_cast<size_t>(getpagesize());
    StackPool pool;

    const size_t Ua = 16 * 1024;
    const size_t Ub = 16 * 1024 + P;

    auto* a1 = pool.Apply(Ua, true);
    auto* b1 = pool.Apply(Ub, true);
    BOOST_REQUIRE(a1 != nullptr);
    BOOST_REQUIRE(b1 != nullptr);
    BOOST_CHECK_EQUAL(a1->UseableSize(), Ua);
    BOOST_CHECK_EQUAL(b1->UseableSize(), Ub);
    BOOST_CHECK(a1 != b1);      // 不同尺寸不共用对象

    const void* const a_addr = a1->MemChunkBegin();
    const void* const b_addr = b1->MemChunkBegin();

    pool.Release(a1);
    pool.Release(b1);

    auto* a2 = pool.Apply(Ua, true);
    auto* b2 = pool.Apply(Ub, true);
    BOOST_CHECK(a2 == a1);
    BOOST_CHECK(b2 == b1);
    BOOST_CHECK(a2->MemChunkBegin() == a_addr);
    BOOST_CHECK(b2->MemChunkBegin() == b_addr);
    BOOST_CHECK_EQUAL(a2->UseableSize(), Ua);
    BOOST_CHECK_EQUAL(b2->UseableSize(), Ub);

    /* 两个不同的请求归一化到同一 U（P+1 与 2P）必须落同一桶 */
    auto* c1 = pool.Apply(P + 1, true);
    BOOST_REQUIRE(c1 != nullptr);
    BOOST_CHECK_EQUAL(c1->UseableSize(), 2 * P);
    const void* const c_addr = c1->MemChunkBegin();
    pool.Release(c1);
    pool.Release(a2);
    pool.Release(b2);

    auto* c2 = pool.Apply(2 * P, true);
    BOOST_REQUIRE(c2 != nullptr);
    BOOST_CHECK_EQUAL(c2->UseableSize(), 2 * P);
    BOOST_CHECK(c2->MemChunkBegin() == c_addr);
    pool.Release(c2);

    /* 保护属性不串桶：同 U 的有保护 / 无保护同时借出，交错归还再申请 */
    auto* prot = pool.Apply(Ua, true);
    auto* unprot = pool.Apply(Ua, false);
    BOOST_REQUIRE(prot != nullptr);
    BOOST_REQUIRE(unprot != nullptr);
    BOOST_CHECK(prot->IsProtected());
    BOOST_CHECK(!unprot->IsProtected());
    BOOST_CHECK_EQUAL(prot->UseableSize(), Ua);
    BOOST_CHECK_EQUAL(unprot->UseableSize(), Ua);
    BOOST_CHECK_EQUAL(prot->MemChunkSize(), Ua + P);
    BOOST_CHECK_EQUAL(unprot->MemChunkSize(), Ua);
    BOOST_CHECK(prot->MemChunkBegin() != unprot->MemChunkBegin());

    pool.Release(unprot);
    pool.Release(prot);
    auto* unprot2 = pool.Apply(Ua, false);
    auto* prot2 = pool.Apply(Ua, true);
    BOOST_CHECK(unprot2 == unprot);
    BOOST_CHECK(prot2 == prot);
    BOOST_CHECK(!unprot2->IsProtected());
    BOOST_CHECK(prot2->IsProtected());
    pool.Release(prot2);
    pool.Release(unprot2);

    /* 归还按栈自身属性：借出后改动全局默认（含保护开关）不得改变回桶归属 */
    auto* owned = pool.Apply(Ua, true);
    BOOST_REQUIRE(owned != nullptr);
    g_bbt_coroutine_config->m_cfg_stack_size = 99 * 1024;
    g_bbt_coroutine_config->m_cfg_stack_protect = false;
    pool.Release(owned);            // 不读全局配置
    g_bbt_coroutine_config->m_cfg_stack_size = guard.stack_size;
    g_bbt_coroutine_config->m_cfg_stack_protect = guard.stack_protect;

    auto* again = pool.Apply(Ua, true);
    BOOST_CHECK(again == owned);    // 回了原桶；若按全局配置回桶就会被当新桶销毁

    BOOST_CHECK(again->IsProtected());
    pool.Release(again);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);

    const size_t released = pool.ReleaseUnused();
    BOOST_CHECK(released > 0);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 0u);
}

// 热缓存按 LIFO 复用刚归还的栈；同桶多个对象借还后仍保持唯一所有权和准确计数。
BOOST_AUTO_TEST_CASE(t_pool_reuses_recently_returned_stack)
{
    using bbt::coroutine::detail::StackPool;

    ConfigGuard guard;
    g_bbt_coroutine_config->m_cfg_static_coroutine = 0;
    StackPool pool;
    auto* first = pool.Apply(4096, false);
    auto* second = pool.Apply(4096, false);
    BOOST_REQUIRE(first != nullptr);
    BOOST_REQUIRE(second != nullptr);
    BOOST_REQUIRE(first != second);
    pool.Release(first);
    pool.Release(second);

    auto* recent = pool.Apply(4096, false);
    auto* older = pool.Apply(4096, false);
    BOOST_CHECK(recent == second);
    BOOST_CHECK(older == first);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 2u);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 2u);
    BOOST_CHECK_EQUAL(pool.ReleaseUnused(), 0u);
    pool.Release(recent);
    pool.Release(older);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);
    BOOST_CHECK_EQUAL(pool.ReleaseUnused(), 2u);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 0u);
}

// S7：容量约束是跨桶的「同时借出 + 已预留」，检查与预留原子完成；没有单桶乘法放大。
BOOST_AUTO_TEST_CASE(t_pool_capacity_is_cross_bucket_and_atomic)
{
    using bbt::coroutine::detail::StackPool;

    ConfigGuard guard;
    const size_t kMax = 4;
    g_bbt_coroutine_config->m_cfg_max_coroutine = kMax;

    StackPool pool;
    const size_t kThreads = 8;
    const size_t kSizes[kThreads] = {
        8 * 1024, 12 * 1024, 16 * 1024, 20 * 1024,
        24 * 1024, 28 * 1024, 32 * 1024, 40 * 1024};

    Barrier start_gate(kThreads);
    Barrier hold_gate(kThreads);
    std::atomic<size_t> granted{0};
    std::vector<StackPool::ItemType*> held(kThreads, nullptr);
    std::vector<std::thread> workers;

    for (size_t i = 0; i < kThreads; ++i)
    {
        workers.emplace_back([&, i]() {
            start_gate.Wait();
            StackPool::ItemType* item = pool.Apply(kSizes[i], true);
            held[i] = item;
            if (item != nullptr)
                granted.fetch_add(1, std::memory_order_relaxed);
            hold_gate.Wait();       // 成功者持有到第二屏障，失败的也等到齐
            if (item != nullptr)
                pool.Release(item);
        });
    }
    for (auto& worker : workers)
        worker.join();

    BOOST_CHECK_EQUAL(granted.load(), kMax);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);

    /* 释放后可再次申请 */
    auto* again = pool.Apply(8 * 1024, true);
    BOOST_REQUIRE(again != nullptr);
    pool.Release(again);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);
    BOOST_CHECK(pool.ReleaseUnused() >= 1);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 0u);
}

// S8：多线程混合借还 + 回收线程并发；借出对象不得被销毁或重复借出，计数不下溢、最终回零。
BOOST_AUTO_TEST_CASE(t_pool_concurrent_borrow_release_and_reclaim)
{
    using bbt::coroutine::detail::StackPool;

    ConfigGuard guard;
    g_bbt_coroutine_config->m_cfg_max_coroutine = 64;

    StackPool pool;
    const size_t kThreads = 4;
    const size_t kIters = 150;
    const size_t kSizes[4] = {8 * 1024, 12 * 1024, 16 * 1024, 24 * 1024};

    auto* held = pool.Apply(16 * 1024, true);
    auto* spare = pool.Apply(40 * 1024, true);
    BOOST_REQUIRE(held != nullptr && spare != nullptr);
    held->StackTop()[-1] = 0x5a;
    pool.Release(spare); // 保证第一轮有空闲对象可真实销毁，不靠调度运气或 5s 定时器
    Barrier borrowed_gate(kThreads + 1);
    Barrier reclaimed_gate(kThreads + 1);
    std::atomic<int>  violations{0};
    std::atomic<bool> stop_reclaim{false};
    std::atomic<size_t> reclaimed{0};
    std::mutex        live_mutex;
    std::set<StackPool::ItemType*> live{held};

    std::thread reclaimer([&]() {
        if (!borrowed_gate.Wait())
            violations.fetch_add(1);
        if (pool.GetCurCoNum() != kThreads + 1)
            violations.fetch_add(1);
        reclaimed.fetch_add(pool.ReleaseUnused()); // 此时主线程和四个 worker 均持有借出栈
        if (!reclaimed_gate.Wait())
            violations.fetch_add(1);
        while (!stop_reclaim.load(std::memory_order_acquire))
        {
            reclaimed.fetch_add(pool.ReleaseUnused());
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> workers;
    for (size_t t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([&, t]() {
            for (size_t i = 0; i < kIters; ++i)
            {
                auto* item = pool.Apply(kSizes[(t + i) % 4], true);
                if (i == 0)
                {
                    if (!borrowed_gate.Wait())
                        violations.fetch_add(1);
                    if (!reclaimed_gate.Wait())
                        violations.fetch_add(1);
                }
                if (item == nullptr)
                {
                    violations.fetch_add(1);    // 总借出远低于上限，不应失败
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lk(live_mutex);
                    if (!live.insert(item).second)
                        violations.fetch_add(1000);     // 同一对象被重复借出
                }

                /* 借出期间栈必须有效可写 */
                item->StackTop()[-1] = static_cast<char>(i & 0x7f);

                {
                    std::lock_guard<std::mutex> lk(live_mutex);
                    live.erase(item);
                }
                pool.Release(item);
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    stop_reclaim.store(true, std::memory_order_release);
    reclaimer.join();

    BOOST_CHECK_EQUAL(violations.load(), 0);
    BOOST_CHECK_GT(reclaimed.load(), 0u);
    BOOST_TEST_MESSAGE("S8 actual concurrent reclaimed stacks=" << reclaimed.load());
    {
        std::lock_guard<std::mutex> lk(live_mutex);
        BOOST_CHECK_EQUAL(live.size(), 1u);
        BOOST_CHECK(live.count(held) == 1);
    }
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 1u);

    /* 从启动回收线程前一直持有的栈必须跨过真实销毁仍可写、数据未污染。 */
    BOOST_CHECK_EQUAL(static_cast<unsigned char>(held->StackTop()[-1]), 0x5au);
    pool.Release(held);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);

    BOOST_CHECK(pool.ReleaseUnused() > 0);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 0u);
}

// S11：预热创建 min(预热数, 全池上限) 个彼此独立的默认栈（不是复用循环），
// 其他桶不复制预热；构造失败路径必须清理已建对象且错误可观察。
BOOST_AUTO_TEST_CASE(t_pool_preload_creates_distinct_default_stacks)
{
    using bbt::coroutine::detail::Stack;
    using bbt::coroutine::detail::StackPool;

    ConfigGuard guard;
    auto* cfg = g_bbt_coroutine_config.get();
    cfg->m_cfg_stack_size = 16 * 1024;
    cfg->m_cfg_stack_protect = true;
    cfg->m_cfg_max_coroutine = 2;
    cfg->m_cfg_static_coroutine = 5;        // 预热数 > 上限 → 裁剪到 2

    const size_t global_alloc = g_bbt_stackpoll->AllocSize();
    const size_t global_cur   = g_bbt_stackpoll->GetCurCoNum();

    StackPool pool;
    BOOST_CHECK_EQUAL(pool.AllocSize(), 2u);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);

    auto* other = pool.Apply(32 * 1024, true);
    BOOST_REQUIRE(other != nullptr);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 3u);        // 只有默认桶预热，其他桶是真实新建
    pool.Release(other);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);

    auto* a = pool.Apply();     // 默认入口：默认 U/保护桶
    auto* b = pool.Apply();
    BOOST_REQUIRE(a != nullptr);
    BOOST_REQUIRE(b != nullptr);
    BOOST_CHECK(a != b);                                        // 独立对象
    BOOST_CHECK(a->MemChunkBegin() != b->MemChunkBegin());
    BOOST_CHECK_EQUAL(a->UseableSize(), 16 * 1024);
    BOOST_CHECK(a->IsProtected());

    pool.Release(a);
    pool.Release(b);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);
    BOOST_CHECK(pool.ReleaseUnused() >= 1);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 0u);

    /* 构造失败（memalign 空返回）必须可观察，且不留半份状态 */
    {
        bool threw_bad_alloc = false;
        Stack::_TestFailNextAlloc();
        try { StackPool failing; (void)failing; }
        catch (const std::bad_alloc&) { threw_bad_alloc = true; }
        BOOST_CHECK(threw_bad_alloc);
    }
    /* 失败后同一配置仍能正常预热（证明没有残留的共享状态） */
    {
        StackPool recovered;
        BOOST_CHECK_EQUAL(recovered.AllocSize(), 2u);
    }

    /* 入队失败：已创建对象全部丢弃，不留半份计数（真实穿过 Push 结果检查） */
    {
        StackPool publish;
        BOOST_CHECK_EQUAL(publish.AllocSize(), 2u);
    }
    {
        cfg->m_cfg_static_coroutine = 4;
        cfg->m_cfg_max_coroutine = 4;
        StackPool::_TestFailNextPoolPush();
        StackPool partial;
        BOOST_CHECK_EQUAL(partial.AllocSize(), 0u);
    }

    BOOST_CHECK_EQUAL(g_bbt_stackpoll->AllocSize(), global_alloc);
    BOOST_CHECK_EQUAL(g_bbt_stackpoll->GetCurCoNum(), global_cur);
}

// S12：显式清空只回收空闲（借出仍有效）；定时回收目标是全池 max(最低保留数, 平均目标)，
// 不为每桶复制最低数。
BOOST_AUTO_TEST_CASE(t_pool_release_unused_keeps_borrowed_and_onupdate_target)
{
    using bbt::coroutine::detail::StackPool;

    ConfigGuard guard;
    StackPool pool;

    /* 数种尺寸缓存 + 一个持有的借出栈 */
    auto* held = pool.Apply(16 * 1024, true);
    auto* a1 = pool.Apply(8 * 1024, true);
    auto* a2 = pool.Apply(8 * 1024, true);
    auto* b1 = pool.Apply(24 * 1024, true);
    auto* b2 = pool.Apply(24 * 1024, true);
    BOOST_REQUIRE(held && a1 && a2 && b1 && b2);
    pool.Release(a1);
    pool.Release(a2);
    pool.Release(b1);
    pool.Release(b2);

    const size_t before = pool.AllocSize();
    BOOST_REQUIRE_EQUAL(before, 5u);
    const size_t released = pool.ReleaseUnused();
    BOOST_CHECK_EQUAL(released, before - 1);        // 只释放空闲，不碰借出的 held
    BOOST_CHECK_EQUAL(pool.AllocSize(), 1u);

    /* 借出栈跨显式清缓存仍有效 */
    held->StackTop()[-1] = 0x22;
    BOOST_CHECK_EQUAL(static_cast<unsigned char>(held->StackTop()[-1]), 0x22u);
    BOOST_CHECK_EQUAL(held->UseableSize(), 16 * 1024);

    pool.Release(held);
    BOOST_CHECK_EQUAL(pool.ReleaseUnused(), 1u);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 0u);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);

    /* 定时回收：默认最低保留数不限制显式清缓存，但限制定时回收的下界。
     * 目标 = max(全池最低保留数, floor((rate+1)*平均))，此处为 max(2, 0) = 2。 */
    g_bbt_coroutine_config->m_cfg_stackpool_min_alloc_size = 2;
    auto* x1 = pool.Apply(8 * 1024, true);
    auto* x2 = pool.Apply(8 * 1024, true);
    auto* x3 = pool.Apply(8 * 1024, true);
    auto* y1 = pool.Apply(24 * 1024, true);
    auto* y2 = pool.Apply(24 * 1024, true);
    auto* y3 = pool.Apply(24 * 1024, true);
    BOOST_REQUIRE(x1 && x2 && x3 && y1 && y2 && y3);
    pool.Release(x1);
    pool.Release(x2);
    pool.Release(x3);
    pool.Release(y1);
    pool.Release(y2);
    pool.Release(y3);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 6u);

    /* 等既有调整间隔（m_cfg_stackpool_adjust_interval 是编译期常量 5000ms） */
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (pool.AllocSize() > 2 && std::chrono::steady_clock::now() < deadline)
    {
        pool.OnUpdate();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    BOOST_CHECK_EQUAL(pool.AllocSize(), 2u);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);
}

// S10：真实窄注入——memalign、建立保护、缓存入队分别失败；预留回滚、计数不残留、
// 后续申请成功。注入穿过真实错误检查分支，不是伪造业务结果。
BOOST_AUTO_TEST_CASE(t_pool_injected_failures_rollback_and_recover)
{
    using bbt::coroutine::detail::Context;
    using bbt::coroutine::detail::Stack;
    using bbt::coroutine::detail::StackPool;

    ConfigGuard guard;
    StackPool pool;

    /* 1) memalign 空返回 → bad_alloc，预留回滚，后续申请成功 */
    Stack::_TestFailNextAlloc();
    BOOST_CHECK_THROW(pool.Apply(8 * 1024, false), std::bad_alloc);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 0u);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);

    auto* ok = pool.Apply(8 * 1024, false);
    BOOST_REQUIRE(ok != nullptr);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 1u);
    pool.Release(ok);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);

    /* 2) 建立保护失败 → 带 errno 的 system_error；该对象不记账 */
    Stack::_TestFailNextApplyProtect();
    bool caught_system_error = false;
    try
    {
        pool.Apply(8 * 1024, true);
    }
    catch (const std::system_error& e)
    {
        caught_system_error = (e.code().value() == EACCES);
    }
    BOOST_CHECK(caught_system_error);
    BOOST_CHECK_EQUAL(pool.AllocSize(), 1u);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);

    /* 3) Context 入口同样不发布半成品（走全局池） */
    Stack::_TestFailNextAlloc();
    BOOST_CHECK_THROW(Context(4 * 1024, [](){}, false), std::bad_alloc);
    BOOST_CHECK_EQUAL(g_bbt_stackpoll->GetCurCoNum(), 0u);

    /* 4) 缓存入队失败：撤回缓存并锁外销毁，计数不残留，后续申请成功 */
    auto* item = pool.Apply(8 * 1024, false);       // 复用步骤 1 归还的空闲对象
    BOOST_REQUIRE(item != nullptr);
    const size_t live_before = pool.AllocSize();
    BOOST_REQUIRE_EQUAL(live_before, 1u);
    StackPool::_TestFailNextPoolPush();
    pool.Release(item);
    BOOST_CHECK_EQUAL(pool.AllocSize(), live_before - 1);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);

    auto* again = pool.Apply(8 * 1024, false);
    BOOST_REQUIRE(again != nullptr);
    pool.Release(again);

    /* 5) 构造失败（callable 拷贝抛出）不借栈、不泄漏 */
    struct ThrowingCallable
    {
        ThrowingCallable() {}
        ThrowingCallable(const ThrowingCallable&) { throw std::runtime_error("copy ctor"); }
        void operator()() const {}
    };

    const size_t alloc_before = pool.AllocSize();
    {
        bool threw = false;
        ThrowingCallable callable;
        try
        {
            bbt::coroutine::detail::Context ctx(8 * 1024, callable, false);
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        BOOST_CHECK(threw);
    }
    BOOST_CHECK_EQUAL(g_bbt_stackpoll->GetCurCoNum(), 0u);
    BOOST_CHECK_EQUAL(pool.AllocSize(), alloc_before);
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

    const size_t baseline = g_bbt_stackpoll->GetCurCoNum();

    g_probe_destroy_count.store(0, std::memory_order_relaxed);
    g_probe_cur_at_destroy.store(static_cast<size_t>(-1), std::memory_order_relaxed);

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
    BOOST_CHECK_EQUAL(pool.AllocSize(), 0u);
    BOOST_CHECK_EQUAL(pool.GetCurCoNum(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
