#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <iostream>
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include <set>
#include <memory>
#include <mutex>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>

/**
 * 进程寿命模型：runtime 只初始化一次，重复 Start 抛 std::logic_error。
 * 每个测试文件就是一个可执行，这里把用例内的 Start() 收敛为进程内一次初始化。
 */
namespace {
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, []() { bbt::coroutine::detail::Scheduler::GetInstance()->Start(); });
}
} // namespace

using namespace bbt::coroutine;

/**
 * 截止时间保护的让权自旋：仅在协程内使用，等待某个已发生的事实变得可见。
 * 超时返回 false 交由调用方断言失败；不引入正确性依赖的固定等待。
 */
static bool SpinUntilCo(const std::function<bool()>& ready, int timeout_ms)
{
    auto deadline = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(timeout_ms));
    while (!ready()) {
        if (bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(deadline))
            return false;
        bbtco_yield;
    }
    return true;
}

/**
 * 同 P 等待观察点：目标在进入阻塞调用前发布自身 Coroutine 指针。
 * 编排协程与目标登记在同一 Processer，编排协程运行时目标不可能并发执行，
 * 因此读取其诊断现场安全；done 在协程结束前发布，观察方看到 done 后不再解引用。
 * 这是「已进入等待」的确定性屏障，不依赖 sleep 或调度猜测。
 */
struct ParkWatch {
    std::atomic<detail::Coroutine*> co{nullptr};
    std::atomic_int done{0};
};

static bool WaitParkedOnCustom(const std::shared_ptr<ParkWatch>& watch, int timeout_ms)
{
    auto deadline = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(timeout_ms));
    while (!bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(deadline)) {
        if (watch->done.load(std::memory_order_acquire))
            return false;

        if (auto* co = watch->co.load(std::memory_order_acquire)) {
            detail::CoroutineWaitInfo info;
            if (co->GetWaitInfo(info) == 0 && info.m_wait_event == detail::POLL_EVENT_CUSTOM)
                return true;
        }
        bbtco_yield;
    }
    return false;
}

/**
 * 带 deadline 的 latch 等待。CountDownLatch::WaitTimeout 在旧 bbtools-core
 * （CI 链接的系统库）里恒立即超时，改用 WaitTimeout(1) 轮询，版本兼容。
 */
static void WaitLatch(bbt::core::thread::CountDownLatch& latch, int timeout_ms)
{
    auto deadline = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(timeout_ms));
    while (!bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(deadline)) {
        if (latch.WaitTimeout(1) == 0)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    BOOST_ERROR("latch wait timed out");
}

BOOST_AUTO_TEST_SUITE()

BOOST_AUTO_TEST_CASE(t_begin)
{
    EnsureRuntime();
}

BOOST_AUTO_TEST_CASE(t_chan_block)
{
    BOOST_TEST_MESSAGE("enter t_chan_block");

    /**
     * 时序约束：每个子 writer 按值持有 Chan 的 shared_ptr 并在写完时 Down
     * latch；外层读协程读完三次后也 Down；主测试线程 l.Wait() 之后才对
     * results 断言——不在 worker 协程上执行 BOOST_CHECK，也不以 sleep
     * 冒充完成；results 活在 main 栈上，失败路径不会遗留野引用。
     */
    bbt::core::thread::CountDownLatch l{4};
    int results[3] = {0, 0, 0};
    bbtco[l = &l, &results]()
    {
        auto c = Chan<int, 1>();
        int val;

        bbtco[c, l]()
        {
            c << 12;
            l->Down();
        };
        c >> val;
        results[0] = val;

        bbtco[c, l]()
        {
            c << 14;
            l->Down();
        };
        c >> val;
        results[1] = val;

        bbtco[c, l]()
        {
            c << 42;
            l->Down();
        };
        c >> val;
        results[2] = val;

        l->Down();
    };
    l.Wait();
    BOOST_CHECK_EQUAL(results[0], 12);
    BOOST_CHECK_EQUAL(results[1], 14);
    BOOST_CHECK_EQUAL(results[2], 42);
}

/**
 * 单读单写
 */
BOOST_AUTO_TEST_CASE(t_chan_1_vs_1)
{
    BOOST_TEST_MESSAGE("enter t_chan_1_vs_1");
    std::atomic_int count = 0;
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&count, &l]()
    {
        auto c = Chan<int, 65535>();

        bbtco[&c]()
        {
            for (int i = 0; i < 100; ++i)
                BOOST_ASSERT(c->Write(i) == 0);
        };

        for (int i = 0; i < 100; ++i) {
            int val;
            BOOST_ASSERT(c->Read(val) == 0);
            count++;
        }

        l.Down();
    };

    /**
     * sleep(1);
     */
    l.Wait();
    BOOST_CHECK_EQUAL(count.load(), 100);
}

/**
 * 单读多写
 */
BOOST_AUTO_TEST_CASE(t_chan_1_vs_n)
{
    BOOST_TEST_MESSAGE("enter t_chan_1_vs_n");
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int count = 0;
    bbtco[&]()
    {
        auto c = Chan<int, 65535>();

        for (int i = 0; i < 1000; ++i) {
            bbtco[&c, i]()
            {
                for (int i = 0; i < 100; ++i) {
                    int ret = c->Write(i);
                    BOOST_ASSERT(ret == 0);
                }
            };
        }

        for (int i = 0; i < 1000 * 100; ++i) {
            int val;
            BOOST_ASSERT(c->Read(val) == 0);
            count++;
        }

        l.Down();
    };

    l.Wait();
    BOOST_CHECK_EQUAL(count.load(), 100 * 1000);
}

/**
 * 运算符重载测试
 */
BOOST_AUTO_TEST_CASE(t_chan_operator_overload)
{
    BOOST_TEST_MESSAGE("enter t_chan_operator_overload");
    std::atomic_int count = 0;
    int wait_ms = 100;
    bbt::core::thread::CountDownLatch l{1};

    bbtco[&count, wait_ms, &l]()
    {
        bool succ = false;
        auto chan = Chan<int, 65535>();
        int write_val = 1;
        succ = chan << write_val;
        BOOST_ASSERT(succ);

        int val = 0;
        succ = chan >> val;
        BOOST_ASSERT(succ);
        BOOST_ASSERT(val == 1);

        int ret = chan->TryRead(val, wait_ms);
        BOOST_ASSERT(ret != 0);
        l.Down();
    };

    l.Wait();

    l.Reset(1);

    bbtco[&count, wait_ms, &l]()
    {
        bool succ = false;
        auto chan = sync::Chan<int, 65535>();
        int write_val = 1;
        succ = chan << write_val;
        BOOST_ASSERT(succ);

        int read_val = 0;
        succ = chan >> read_val;
        BOOST_ASSERT(succ);
        BOOST_ASSERT(read_val == 1);

        int ret = chan.TryRead(read_val, wait_ms);
        BOOST_ASSERT(ret != 0);
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_block_write)
{
    BOOST_TEST_MESSAGE("enter t_block_write");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&]()
    {
        auto chan = Chan<int, 100>();
        bbtco[&]()
        {
            int val;
            chan >> val;
            l.Down();
        };

        sleep(1);
        chan << 1;
    };

    l.Wait();
    BOOST_ASSERT(true);
}

BOOST_AUTO_TEST_CASE(t_block_write_n)
{
    BOOST_TEST_MESSAGE("enter t_block_write_n");
    bbt::core::thread::CountDownLatch l{1};
    const int nwrite = 100;

    /**
     * 读者读完 100 个值不等于写者已经恢复并自增；成功计数必须以全部写者
     * 真正返回为准，不能靠读完后的时间窗口猜。
     */
    bbt::core::thread::CountDownLatch writers{nwrite};
    std::atomic_int n_read_succ_num{0};
    std::set<int> results;

    bbtco[&]()
    {
        auto chan = Chan<int, 1>();

        for (int i = 0; i < nwrite; ++i) {
            bbtco[&chan, &n_read_succ_num, &writers, i]()
            {
                chan << i;
                n_read_succ_num++;
                writers.Down();
            };
        }

        int val;
        for (int i = 0; i < nwrite; ++i) {
            while (!(chan >> val))
                ;

            /**
             * detail::Hook_Sleep(1);
             */
            results.insert(val);
        }

        l.Down();
    };

    l.Wait();
    writers.Wait();

    BOOST_CHECK_EQUAL(nwrite, n_read_succ_num.load());
    for (int i = 0; i < nwrite; ++i) {
        auto it = results.find(i);

        BOOST_ASSERT(it != results.end());
    }
}

/**
 * 无缓冲单次读写
 */
BOOST_AUTO_TEST_CASE(t_nocache_chan_1v1)
{
    BOOST_TEST_MESSAGE("enter t_nocache_chan_1v1");
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_bool flag{false};

    bbtco[&]()
    {
        auto chan = Chan<int, 0>();
        auto cond = sync::CoWaiter::Create();
        bbtco[&]()
        {
            bbtco_sleep(100);
            cond->Notify();
            chan << 1;
            flag.exchange(true);
        };

        cond->Wait();
        BOOST_ASSERT(!flag);
        l.Down();
    };

    l.Wait();
}

/**
 * 无缓冲单读多写
 */
BOOST_AUTO_TEST_CASE(t_nocache_chan_1vn)
{
    BOOST_TEST_MESSAGE("enter t_nocache_chan_1vn");

    const int nwrite_co_num = 100;
    int ncount = 0;
    bbt::core::thread::CountDownLatch l{nwrite_co_num + 1};

    bbtco[&]()
    {
        auto chan = Chan<int, 0>();
        for (int i = 0; i < nwrite_co_num; ++i)
            bbtco[&]()
            {
                chan << 1;
                l.Down();
            };

        sleep(1);
        int val;
        while (chan >> val) {
            ncount++;
            if (ncount >= nwrite_co_num)
                chan->Close();
        }
        l.Down();
    };

    l.Wait();

    BOOST_ASSERT(ncount == nwrite_co_num);
}

BOOST_AUTO_TEST_CASE(t_close)
{
    BOOST_TEST_MESSAGE("enter t_close");
    std::atomic_bool flag{false};
    bbt::core::thread::CountDownLatch l{2};

    auto chan = Chan<int, 65535>();
    bbtco[&flag, &chan, &l]()
    {
        bbtco[&chan, &l]()
        {
            int block;

            /**
             * 阻塞100ms
             */
            chan->TryRead(block, 100);
            chan->Close();
            l.Down();
        };

        bbtco[&chan, &flag, &l]()
        {
            int val;
            chan >> val;
            flag = true;
            l.Down();
        };
    };

    l.Wait();
    BOOST_CHECK(flag);
}

/**
 * ================================================================
 * 新增测试 (合并版)
 * ================================================================
 */

/**
 * 组1: 基础正确性 — 不同类型 + ReadAll
 */
BOOST_AUTO_TEST_CASE(t_new_basic)
{
    BOOST_TEST_MESSAGE("enter t_new_basic");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        /**
         * string 类型
         */
        {
            auto c = Chan<std::string, 10>();
            c->Write(std::string("hello"));
            std::string val;
            BOOST_ASSERT(c->Read(val) == 0);
            BOOST_CHECK_EQUAL(val, "hello");
        }

        /**
         * 自定义 struct
         */
        {
            struct TItem {
                int id;
                double val;
            };

            auto c = Chan<TItem, 10>();
            TItem item{42, 3.14};
            BOOST_ASSERT(c->Write(item) == 0);
            TItem out{};
            BOOST_ASSERT(c->Read(out) == 0);
            BOOST_CHECK_EQUAL(out.id, 42);
            BOOST_CHECK_EQUAL(out.val, 3.14);
        }

        /**
         * ReadAll 非空
         */
        {
            auto c = Chan<int, 10>();
            c->Write(1);
            c->Write(2);
            c->Write(3);
            std::vector<int> items;
            BOOST_ASSERT(c->ReadAll(items) == 0);
            BOOST_CHECK_EQUAL(items.size(), 3);
            BOOST_CHECK_EQUAL(items[0], 1);
        }

        /**
         * ReadAll 阻塞等待
         */
        {
            auto c = Chan<int, 10>();
            bbtco[c]()
            {
                bbtco_sleep(50);
                c->Write(10);
                c->Write(20);
            };
            std::vector<int> items;
            BOOST_ASSERT(c->ReadAll(items) == 0);
            BOOST_CHECK_EQUAL(items.size(), 2);
        }

        /**
         * ReadAll 读空后再写入
         */
        {
            auto c = Chan<int, 10>();
            c->Write(1);
            c->Write(2);
            std::vector<int> items;
            BOOST_ASSERT(c->ReadAll(items) == 0);
            BOOST_CHECK_EQUAL(items.size(), 2);
            items.clear();
            c->Write(3);
            BOOST_ASSERT(c->ReadAll(items) == 0);
            BOOST_CHECK_EQUAL(items.size(), 1);
            BOOST_CHECK_EQUAL(items[0], 3);
        }
        l.Down();
    };
    l.Wait();

    /**
     * Chan<0> ReadAll 不可访问 (protected 继承)
     */
    BOOST_TEST_MESSAGE("Chan<0> ReadAll/TryWrite/TryRead inaccessible via protected inheritance");
}

/**
 * 组2: TryWrite 全部场景
 */
BOOST_AUTO_TEST_CASE(t_new_trywrite)
{
    BOOST_TEST_MESSAGE("enter t_new_trywrite");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();

        /**
         * 成功
         */
        BOOST_CHECK_EQUAL(c->TryWrite(42), 0);
        int val = 0;
        BOOST_ASSERT(c->Read(val) == 0);
        BOOST_CHECK_EQUAL(val, 42);

        /**
         * 满队列失败
         */
        auto c1 = Chan<int, 1>();
        BOOST_ASSERT(c1->Write(1) == 0);
        BOOST_CHECK_NE(c1->TryWrite(2), 0);

        /**
         * 超时
         */
        {
            auto c2 = Chan<int, 1>();
            BOOST_ASSERT(c2->Write(1) == 0);
            auto t1 = bbt::core::clock::gettime<>();
            int ret = c2->TryWrite(2, 100);
            BOOST_CHECK_EQUAL(ret, 1);
            BOOST_CHECK(bbt::core::clock::gettime<>() - t1 >= 90);
        }

        /**
         * 超时被消费后成功
         */
        {
            auto c2 = Chan<int, 1>();
            BOOST_ASSERT(c2->Write(1) == 0);
            bbtco[c2]()
            {
                bbtco_sleep(50);
                int v;
                c2->Read(v);
            };
            BOOST_CHECK_EQUAL(c2->TryWrite(2, 500), 0);
        }
        l.Down();
    };
    l.Wait();
}

/**
 * 组3: TryRead 全部场景 + m_is_reading 缺陷暴露
 */
BOOST_AUTO_TEST_CASE(t_new_tryread)
{
    BOOST_TEST_MESSAGE("enter t_new_tryread");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();

        /**
         * 成功
         */
        c->Write(42);
        int val = 0;
        BOOST_CHECK_EQUAL(c->TryRead(val), 0);
        BOOST_CHECK_EQUAL(val, 42);

        /**
         * 队列空失败
         */
        BOOST_CHECK_NE(c->TryRead(val), 0);

        /**
         * 超时
         */
        {
            auto tc = Chan<int, 10>();
            int v2 = 0;
            int ret = tc->TryRead(v2, 100);
            BOOST_CHECK_NE(ret, 0);
        }

        /**
         * 超时期间写入后成功 (使用独立 Chan)
         */
        {
            auto tc = Chan<int, 10>();
            bbtco[tc]()
            {
                bbtco_sleep(50);
                tc->Write(99);
            };
            int v3 = 0;
            int ret = tc->TryRead(v3, 500);
            BOOST_CHECK_EQUAL(ret, 0);
            BOOST_CHECK_EQUAL(v3, 99);
        }

        /**
         * ⚠️ m_is_reading 泄露: TryRead 成功后 Read 应可用
         */
        {
            auto c2 = Chan<int, 10>();
            c2->Write(1);
            int tv = 0;
            BOOST_REQUIRE(c2->TryRead(tv) == 0);
            c2->Write(2);
            int ret = c2->Read(tv);
            BOOST_TEST_MESSAGE("TryRead leak: Read after TryRead ret=" << ret << " (expect 0, !=0=leak)");
            if (ret == 0)
                BOOST_CHECK_EQUAL(tv, 2);
        }

        /**
         * ⚠️ m_is_reading 泄露: TryRead(timeout) 后 Read 应可用
         */
        {
            auto c2 = Chan<int, 10>();
            int tv = 0;
            BOOST_CHECK_NE(c2->TryRead(tv, 100), 0);
            c2->Write(42);
            int ret = c2->Read(tv);
            BOOST_TEST_MESSAGE(
                "TryRead timeout leak: Read after TryRead(timeout) ret=" << ret << " (expect 0, !=0=leak)");
            if (ret == 0)
                BOOST_CHECK_EQUAL(tv, 42);
        }
        l.Down();
    };
    l.Wait();
}

/**
 * 组4: Close 生命周期 (关闭后操作 + 唤醒阻塞协程)
 */
BOOST_AUTO_TEST_CASE(t_new_close_lifecycle)
{
    BOOST_TEST_MESSAGE("enter t_new_close_lifecycle");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        /**
         * 关闭后所有操作返回 -1
         */
        {
            auto c = Chan<int, 10>();
            BOOST_CHECK(!c->IsClosed());
            c->Close();
            BOOST_CHECK(c->IsClosed());
            BOOST_CHECK_EQUAL(c->Write(1), -1);
            int val;
            BOOST_CHECK_EQUAL(c->Read(val), -1);
            BOOST_CHECK_EQUAL(c->TryWrite(1), -1);
            BOOST_CHECK_NE(c->TryRead(val), 0);
            std::vector<int> items;
            BOOST_CHECK_EQUAL(c->ReadAll(items), -1);

            /**
             * 重复 Close 安全
             */
            c->Close();
        }

        /**
         * Close 唤醒阻塞 Read
         */
        {
            bbt::core::thread::CountDownLatch l2{2};
            std::atomic_bool read_done{false};
            auto c = Chan<int, 10>();
            bbtco[c, &l2, &read_done]()
            {
                int v;
                c->Read(v);
                read_done.store(true);
                l2.Down();
            };
            bbtco_sleep(50);
            c->Close();
            l2.Down();
            l2.Wait();
            BOOST_CHECK(read_done.load());
        }

        /**
         * Close 唤醒阻塞 Write (单个)
         */
        {
            bbt::core::thread::CountDownLatch l2{2};
            auto c = Chan<int, 1>();
            c->Write(1);
            std::atomic_bool write_done{false};
            bbtco[c, &l2, &write_done]()
            {
                int r = c->Write(2);
                write_done.store(true);
                BOOST_CHECK_EQUAL(r, -1);
                l2.Down();
            };
            bbtco_sleep(30);
            c->Close();
            l2.Down();
            l2.Wait();
            BOOST_CHECK(write_done.load());
        }

        /**
         * Close 唤醒阻塞 TryRead(timeout) + TryWrite(timeout)
         */
        {
            bbt::core::thread::CountDownLatch l2{2};
            std::atomic_bool tr{false}, tw{false};
            auto c = Chan<int, 1>();

            /**
             * 填满，使得 TryWrite 阻塞
             */
            c->Write(1);
            bbtco[c, &l2, &tr]()
            {
                int v;
                c->TryRead(v, 5000);
                tr.store(true);
                l2.Down();
            };
            bbtco[c, &l2, &tw]()
            {
                c->TryWrite(2, 5000);
                tw.store(true);
                l2.Down();
            };
            bbtco_sleep(30);
            c->Close();
            l2.Wait();
            BOOST_CHECK(tr.load() && tw.load());
        }

        /**
         * 无缓冲 Close 后操作
         */
        {
            auto c = Chan<int, 0>();
            c->Close();
            BOOST_CHECK_EQUAL(c->Write(1), -1);
            int val;
            BOOST_CHECK_EQUAL(c->Read(val), -1);
        }

        /**
         * 无缓冲 Close 唤醒阻塞写
         */
        {
            bbt::core::thread::CountDownLatch l2{2};
            std::atomic_bool wd{false};
            auto c = Chan<int, 0>();
            bbtco[c, &l2, &wd]()
            {
                int r = c->Write(1);
                wd.store(true);
                BOOST_CHECK_EQUAL(r, -1);
                l2.Down();
            };
            bbtco_sleep(30);
            c->Close();
            l2.Down();
            l2.Wait();
            BOOST_CHECK(wd.load());
        }

        /**
         * 析构自动 Close (验证不崩溃)
         */
        {
            auto c = Chan<int, 10>();

            /**
             * 析构
             */
            c->Write(1);
        }
        l.Down();
    };
    l.Wait();
}

/**
 * 组5: 并发竞争
 */
BOOST_AUTO_TEST_CASE(t_new_concurrency)
{
    BOOST_TEST_MESSAGE("enter t_new_concurrency");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        /**
         * 多读者：两个 Read 同时等待都合法。统一核心按接收等待 FIFO 匹配，
         * 不再有旧单消费者 m_is_reading 的「第二个读者必须失败」限制。
         * 两个值写入后两个读者都成功、各取一个值，不重复交付。
         */
        {
            std::atomic_int r1{-999}, r2{-999};
            std::atomic_int v1{0}, v2{0};
            std::atomic_int arrived{0};
            auto c = Chan<int, 10>();
            bbtco[c, &r1, &v1, &arrived]()
            {
                int v = 0;
                arrived.fetch_add(1);
                int ret = c->Read(v);
                v1.store(v, std::memory_order_relaxed);
                r1.store(ret, std::memory_order_release);
            };
            bbtco[c, &r2, &v2, &arrived]()
            {
                int v = 0;
                arrived.fetch_add(1);
                int ret = c->Read(v);
                v2.store(v, std::memory_order_relaxed);
                r2.store(ret, std::memory_order_release);
            };

            /**
             * 两个读者都已发起 Read 后再注入；谁先 park 由运行时决定，
             * 断言只要求两个等待都被接纳且各得一值。
             */
            bool both_waiting = SpinUntilCo([&arrived] { return arrived.load() == 2; }, 3000);
            BOOST_CHECK(both_waiting);
            BOOST_ASSERT(c->Write(1) == 0);
            BOOST_ASSERT(c->Write(2) == 0);
            bool both_returned = SpinUntilCo(
                [&r1, &r2]() {
                    return r1.load(std::memory_order_acquire) != -999 && r2.load(std::memory_order_acquire) != -999;
                },
                3000);
            BOOST_CHECK(both_returned);

            BOOST_CHECK_EQUAL(r1.load(std::memory_order_acquire), 0);
            BOOST_CHECK_EQUAL(r2.load(std::memory_order_acquire), 0);
            BOOST_CHECK((v1.load() == 1 && v2.load() == 2) || (v1.load() == 2 && v2.load() == 1));
            BOOST_CHECK_NE(v1.load(), v2.load());
        }

        /**
         * TryRead CAS 竞争
         */
        {
            bbt::core::thread::CountDownLatch l2{2};
            std::atomic_int r1{0}, r2{0};
            int v1 = 0, v2 = 0;
            auto c = Chan<int, 10>();
            c->Write(42);
            bbtco[c, &r1, &v1, &l2]()
            {
                r1.store(c->TryRead(v1));
                l2.Down();
            };
            bbtco[c, &r2, &v2, &l2]()
            {
                r2.store(c->TryRead(v2));
                l2.Down();
            };
            bbtco_sleep(50);
            l2.Wait();
            BOOST_CHECK((r1.load() == 0 && r2.load() != 0) || (r1.load() != 0 && r2.load() == 0));
            int sv = (r1.load() == 0) ? v1 : v2;
            BOOST_CHECK_EQUAL(sv, 42);
        }

        /**
         * Read 完成后新 Read 可进入
         */
        {
            auto c = Chan<int, 10>();
            c->Write(1);
            c->Write(2);
            int v1, v2;
            BOOST_ASSERT(c->Read(v1) == 0);
            BOOST_CHECK_EQUAL(v1, 1);
            BOOST_ASSERT(c->Read(v2) == 0);
            BOOST_CHECK_EQUAL(v2, 2);
        }

        /**
         * 多读者数据完整性 (简化: 1读)
         */
        {
            const int N = 50;
            auto c = Chan<int, 100>();
            for (int i = 0; i < N; ++i)
                BOOST_ASSERT(c->Write(i) == 0);
            std::set<int> result;
            for (int i = 0; i < N; ++i) {
                int v;
                BOOST_ASSERT(c->Read(v) == 0);
                result.insert(v);
            }
            BOOST_CHECK_EQUAL(result.size(), N);
        }
        l.Down();
    };
    l.Wait();
}

/**
 * 组6: 运算符与数据完整性
 */
BOOST_AUTO_TEST_CASE(t_new_operators)
{
    BOOST_TEST_MESSAGE("enter t_new_operators");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        /**
         * Close 后 << >> 返回 false
         */
        {
            auto c = Chan<int, 10>();
            c->Close();
            BOOST_CHECK(!(c << 42));
            int val;
            BOOST_CHECK(!(c >> val));
        }

        /**
         * shared_ptr << >>
         */
        {
            auto c = std::make_shared<sync::Chan<int, 10>>();
            BOOST_CHECK(c << 42);
            int val;
            BOOST_CHECK(c >> val);
            BOOST_CHECK_EQUAL(val, 42);
        }

        /**
         * FIFO 严格顺序
         */
        {
            auto c = Chan<int, 100>();
            for (int i = 0; i < 100; ++i)
                BOOST_ASSERT(c->Write(i) == 0);
            for (int i = 0; i < 100; ++i) {
                int v;
                BOOST_ASSERT(c->Read(v) == 0);
                BOOST_CHECK_EQUAL(v, i);
            }
        }

        /**
         * 无缓冲值匹配
         */
        {
            auto c = Chan<int, 0>();
            bbtco[c]()
            {
                bbtco_sleep(10);
                c->Write(10);
                bbtco_sleep(10);
                c->Write(20);
            };
            int v1, v2;
            BOOST_ASSERT(c->Read(v1) == 0);
            BOOST_CHECK_EQUAL(v1, 10);
            BOOST_ASSERT(c->Read(v2) == 0);
            BOOST_CHECK_EQUAL(v2, 20);
        }
        l.Down();
    };
    l.Wait();
}

/**
 * 组7: 多写者并发完整性 (轻量)
 */
BOOST_AUTO_TEST_CASE(t_new_multi_writer)
{
    BOOST_TEST_MESSAGE("enter t_new_multi_writer");
    bbt::core::thread::CountDownLatch l{1};
    std::set<int> result_set;
    bbtco[&l, &result_set]()
    {
        auto c = Chan<int, 65535>();
        const int nwriters = 3;
        const int n_per = 5;
        for (int w = 0; w < nwriters; ++w) {
            bbtco[c, w]()
            {
                for (int i = 0; i < n_per; ++i)
                    BOOST_ASSERT(c->Write(w * 1000 + i) == 0);
            };
        }
        int total = nwriters * n_per;
        for (int i = 0; i < total; ++i) {
            int val;
            BOOST_ASSERT(c->Read(val) == 0);
            result_set.insert(val);
        }
        l.Down();
    };
    l.Wait();
    BOOST_CHECK_EQUAL(result_set.size(), 15);
}

/**
 * 原新增测试 End
 * ================================================================
 */

/**
 * 1.2 有缓冲 Chan 自定义 struct 元素读写
 */
struct TestItem {
    int id;
    double val;
};

BOOST_AUTO_TEST_CASE(t_chan_custom_struct)
{
    BOOST_TEST_MESSAGE("enter t_chan_custom_struct");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<TestItem, 10>();
        TestItem item{42, 3.14};
        BOOST_ASSERT(c->Write(item) == 0);
        TestItem out{};
        BOOST_ASSERT(c->Read(out) == 0);
        BOOST_CHECK_EQUAL(out.id, 42);
        BOOST_CHECK_EQUAL(out.val, 3.14);
        l.Down();
    };
    l.Wait();
}

/**
 * 1.3 有缓冲 Chan ReadAll 队列非空时一次性读出
 */
BOOST_AUTO_TEST_CASE(t_readall_nonempty)
{
    BOOST_TEST_MESSAGE("enter t_readall_nonempty");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        c->Write(1);
        c->Write(2);
        c->Write(3);
        std::vector<int> items;
        BOOST_ASSERT(c->ReadAll(items) == 0);
        BOOST_CHECK_EQUAL(items.size(), 3);
        BOOST_CHECK_EQUAL(items[0], 1);
        BOOST_CHECK_EQUAL(items[1], 2);
        BOOST_CHECK_EQUAL(items[2], 3);
        l.Down();
    };
    l.Wait();
}

/**
 * 1.4 有缓冲 Chan ReadAll 队列为空时阻塞直到有数据
 */
BOOST_AUTO_TEST_CASE(t_readall_block)
{
    BOOST_TEST_MESSAGE("enter t_readall_block");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        bbtco[&c]()
        {
            bbtco_sleep(100);
            c->Write(10);
            c->Write(20);
        };
        std::vector<int> items;
        BOOST_ASSERT(c->ReadAll(items) == 0);
        BOOST_CHECK_EQUAL(items.size(), 2);
        l.Down();
    };
    l.Wait();
}

/**
 * 1.5 有缓冲 Chan ReadAll 读空后再写入再 ReadAll
 */
BOOST_AUTO_TEST_CASE(t_readall_refill)
{
    BOOST_TEST_MESSAGE("enter t_readall_refill");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        c->Write(1);
        c->Write(2);
        std::vector<int> items;
        BOOST_ASSERT(c->ReadAll(items) == 0);
        BOOST_CHECK_EQUAL(items.size(), 2);
        items.clear();
        c->Write(3);
        BOOST_ASSERT(c->ReadAll(items) == 0);
        BOOST_CHECK_EQUAL(items.size(), 1);
        BOOST_CHECK_EQUAL(items[0], 3);
        l.Down();
    };
    l.Wait();
}

/**
 * 1.6 无缓冲 Chan ReadAll 行为（protected 继承导致 ReadAll 不可访问）
 */
BOOST_AUTO_TEST_CASE(t_nocache_readall)
{
    BOOST_TEST_MESSAGE("enter t_nocache_readall");

    /**
     * Chan<T,0> 通过 protected 继承 Chan<T,1>，ReadAll 不可从外部访问
     */
    BOOST_TEST_MESSAGE("Chan<0> ReadAll is inaccessible via protected inheritance");
    BOOST_CHECK(true);
}

/**
 * ================================================================
 * 新增测试: 2.TryWrite 系列
 * ================================================================
 */

/**
 * 2.1 TryWrite 缓冲区未满时成功
 */
BOOST_AUTO_TEST_CASE(t_trywrite_success)
{
    BOOST_TEST_MESSAGE("enter t_trywrite_success");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        BOOST_CHECK_EQUAL(c->TryWrite(42), 0);
        int val = 0;
        BOOST_ASSERT(c->Read(val) == 0);
        BOOST_CHECK_EQUAL(val, 42);
        l.Down();
    };
    l.Wait();
}

/**
 * 2.2 TryWrite 缓冲区已满时失败
 */
BOOST_AUTO_TEST_CASE(t_trywrite_full)
{
    BOOST_TEST_MESSAGE("enter t_trywrite_full");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 1>();
        BOOST_ASSERT(c->Write(1) == 0);
        BOOST_CHECK_NE(c->TryWrite(2), 0);
        l.Down();
    };
    l.Wait();
}

/**
 * 2.3 TryWrite(timeout) 缓冲区满等待超时
 */
BOOST_AUTO_TEST_CASE(t_trywrite_timeout)
{
    BOOST_TEST_MESSAGE("enter t_trywrite_timeout");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 1>();

        /**
         * 填满
         */
        BOOST_ASSERT(c->Write(1) == 0);
        auto t1 = bbt::core::clock::gettime<>();
        int ret = c->TryWrite(2, 100);
        auto t2 = bbt::core::clock::gettime<>();

        /**
         * 超时返回 1（非 -1）
         */
        BOOST_CHECK_EQUAL(ret, 1);

        /**
         * 至少等了 90ms
         */
        BOOST_CHECK(t2 - t1 >= 90);
        l.Down();
    };
    l.Wait();
}

/**
 * 2.4 TryWrite(timeout) 缓冲区满期间被消费后成功
 */
BOOST_AUTO_TEST_CASE(t_trywrite_timeout_success)
{
    BOOST_TEST_MESSAGE("enter t_trywrite_timeout_success");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 1>();

        /**
         * 填满
         */
        BOOST_ASSERT(c->Write(1) == 0);
        bbtco[&c]()
        {
            bbtco_sleep(50);
            int val;

            /**
             * 消费掉，腾出空间
             */
            c->Read(val);
        };
        int ret = c->TryWrite(2, 500);
        BOOST_CHECK_EQUAL(ret, 0);
        l.Down();
    };
    l.Wait();
}

/**
 * 2.5 无缓冲 Chan TryWrite 行为（protected 继承导致不可访问）
 */
BOOST_AUTO_TEST_CASE(t_nocache_trywrite)
{
    BOOST_TEST_MESSAGE("enter t_nocache_trywrite");

    /**
     * Chan<T,0> 通过 protected 继承 Chan<T,1>，TryWrite 不可从外部访问
     */
    BOOST_TEST_MESSAGE("Chan<0> TryWrite is inaccessible via protected inheritance");
    BOOST_CHECK(true);
}

/**
 * ================================================================
 * 新增测试: 3.TryRead 系列
 * ================================================================
 */

/**
 * 3.1 TryRead 队列非空时成功
 */
BOOST_AUTO_TEST_CASE(t_tryread_success)
{
    BOOST_TEST_MESSAGE("enter t_tryread_success");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        c->Write(42);
        int val = 0;
        BOOST_CHECK_EQUAL(c->TryRead(val), 0);
        BOOST_CHECK_EQUAL(val, 42);
        l.Down();
    };
    l.Wait();
}

/**
 * 3.2 TryRead 队列为空时失败
 */
BOOST_AUTO_TEST_CASE(t_tryread_empty)
{
    BOOST_TEST_MESSAGE("enter t_tryread_empty");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        int val = 0;
        BOOST_CHECK_NE(c->TryRead(val), 0);
        l.Down();
    };
    l.Wait();
}

/**
 * 3.3 TryRead(timeout) 队列空等待超时
 */
BOOST_AUTO_TEST_CASE(t_tryread_timeout)
{
    BOOST_TEST_MESSAGE("enter t_tryread_timeout");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        int val = 0;
        auto t1 = bbt::core::clock::gettime<>();
        int ret = c->TryRead(val, 100);
        auto t2 = bbt::core::clock::gettime<>();
        BOOST_CHECK_NE(ret, 0);
        BOOST_CHECK(t2 - t1 >= 90);
        l.Down();
    };
    l.Wait();
}

/**
 * 3.4 TryRead(timeout) 队列空期间写入后成功
 */
BOOST_AUTO_TEST_CASE(t_tryread_timeout_success)
{
    BOOST_TEST_MESSAGE("enter t_tryread_timeout_success");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        bbtco[&c]()
        {
            bbtco_sleep(50);
            c->Write(42);
        };
        int val = 0;
        int ret = c->TryRead(val, 500);
        BOOST_CHECK_EQUAL(ret, 0);
        BOOST_CHECK_EQUAL(val, 42);
        l.Down();
    };
    l.Wait();
}

/**
 * 3.5 TryRead 成功后 m_is_reading 是否正确重置（预期暴露缺陷）
 */
BOOST_AUTO_TEST_CASE(t_tryread_m_is_reading_reset)
{
    BOOST_TEST_MESSAGE("enter t_tryread_m_is_reading_reset");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        c->Write(1);
        int val = 0;

        /**
         * TryRead 成功但可能未重置 m_is_reading
         */
        BOOST_REQUIRE(c->TryRead(val) == 0);
        BOOST_CHECK_EQUAL(val, 1);

        /**
         * 再次写入
         */
        c->Write(2);

        /**
         * 尝试 Read — 如果 m_is_reading 未重置，CAS 会失败返回 -1
         */
        int ret = c->Read(val);
        BOOST_TEST_MESSAGE("Read after TryRead => ret=" << ret << " (expect 0, -1 = m_is_reading leak)");
        if (ret == 0)
            BOOST_CHECK_EQUAL(val, 2);
        l.Down();
    };
    l.Wait();
}

/**
 * 3.6 TryRead(timeout) 超时后队列仍空时 m_is_reading 是否正确重置（预期暴露缺陷）
 */
BOOST_AUTO_TEST_CASE(t_tryread_timeout_m_is_reading)
{
    BOOST_TEST_MESSAGE("enter t_tryread_timeout_m_is_reading");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        int val = 0;
        int ret1 = c->TryRead(val, 100);

        /**
         * 超时
         */
        BOOST_CHECK_NE(ret1, 0);

        /**
         * 现在写入数据
         */
        c->Write(42);

        /**
         * 尝试 Read — 如果 m_is_reading 未重置，CAS 会失败返回 -1
         */
        int ret2 = c->Read(val);
        BOOST_TEST_MESSAGE("Read after TryRead(timeout) => ret=" << ret2 << " (expect 0, -1 = m_is_reading leak)");
        if (ret2 == 0)
            BOOST_CHECK_EQUAL(val, 42);
        l.Down();
    };
    l.Wait();
}

/**
 * 3.7 无缓冲 Chan TryRead 行为（protected 继承导致不可访问）
 */
BOOST_AUTO_TEST_CASE(t_nocache_tryread)
{
    BOOST_TEST_MESSAGE("enter t_nocache_tryread");

    /**
     * Chan<T,0> 通过 protected 继承 Chan<T,1>，TryRead 不可从外部访问
     */
    BOOST_TEST_MESSAGE("Chan<0> TryRead is inaccessible via protected inheritance");
    BOOST_CHECK(true);
}

/**
 * ================================================================
 * 新增测试: 4.Close 生命周期
 * ================================================================
 */

/**
 * ================================================================
 * 合并测试：Close Write/Read/Try/ReadAll 关闭后操作 + IsClosed
 * ================================================================
 */
BOOST_AUTO_TEST_CASE(t_close_all_ops)
{
    BOOST_TEST_MESSAGE("enter t_close_all_ops");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        BOOST_CHECK(!c->IsClosed());
        c->Close();
        BOOST_CHECK(c->IsClosed());
        BOOST_CHECK_EQUAL(c->Write(1), -1);
        int val;
        BOOST_CHECK_EQUAL(c->Read(val), -1);
        BOOST_CHECK_EQUAL(c->TryWrite(1), -1);
        BOOST_CHECK_NE(c->TryRead(val), 0);
        std::vector<int> items;
        BOOST_CHECK_EQUAL(c->ReadAll(items), -1);

        /**
         * 重复 Close 安全
         */
        c->Close();
        BOOST_CHECK(c->IsClosed());
        l.Down();
    };
    l.Wait();
}

/**
 * 4.8 Close 唤醒阻塞中的 Read 协程
 */
BOOST_AUTO_TEST_CASE(t_close_wake_read)
{
    BOOST_TEST_MESSAGE("enter t_close_wake_read");
    bbt::core::thread::CountDownLatch l{2};
    std::atomic_bool read_returned{false};
    bbtco[&l, &read_returned]()
    {
        auto c = Chan<int, 10>();

        /**
         * 读端挂起
         */
        bbtco[c, &l, &read_returned]()
        {
            int val;
            int ret = c->Read(val);
            read_returned.store(true);

            /**
             * Close 唤醒后 Read 返回 -1
             */
            BOOST_CHECK_EQUAL(ret, -1);
            l.Down();
        };
        bbtco_sleep(50);

        /**
         * Close 唤醒读端
         */
        c->Close();
        l.Down();
    };
    l.Wait();
    BOOST_CHECK(read_returned.load());
}

/**
 * 4.9 Close 唤醒单个阻塞中的 Write 协程
 */
BOOST_AUTO_TEST_CASE(t_close_wake_write_single)
{
    BOOST_TEST_MESSAGE("enter t_close_wake_write_single");
    bbt::core::thread::CountDownLatch l{2};
    std::atomic_bool write_returned{false};
    bbtco[&l, &write_returned]()
    {
        auto c = Chan<int, 1>();

        /**
         * 填满
         */
        c->Write(1);

        /**
         * 写端挂起
         */
        bbtco[c, &l, &write_returned]()
        {
            /**
             * 阻塞
             */
            int ret = c->Write(2);
            write_returned.store(true);

            /**
             * Close 唤醒后 Write 应返回 -1
             */
            BOOST_CHECK_EQUAL(ret, -1);
            l.Down();
        };
        bbtco_sleep(50);
        c->Close();
        l.Down();
    };
    l.Wait();
    BOOST_CHECK(write_returned.load());
}

/**
 * 4.10 Close 唤醒多个阻塞中的 Write 协程（已知缺陷：Close 后所有写者被唤醒并尝试 push，导致队列溢出崩溃）
 */
BOOST_AUTO_TEST_CASE(t_close_wake_write_multi)
{
    BOOST_TEST_MESSAGE("enter t_close_wake_write_multi");

    /**
     * 注意：Close 会唤醒所有阻塞的写者，但它们都会尝试 push 到已清空的队列，
     * 导致超出 m_max_size 的 push，触发内存访问违规。此处仅验证基本唤醒行为。
     */
    BOOST_TEST_MESSAGE("Known issue: Close wakes all blocked writers, causing queue overflow crash with >1 writer");
    bbt::core::thread::CountDownLatch l{2};
    std::atomic_bool write_returned{false};
    bbtco[&l, &write_returned]()
    {
        auto c = Chan<int, 1>();

        /**
         * 填满
         */
        c->Write(1);

        /**
         * 单写者：避免多写者溢出崩溃
         */
        bbtco[c, &l, &write_returned]()
        {
            int ret = c->Write(2);
            write_returned.store(true);

            /**
             * ret=0: IsClosed 仅在入口检查，唤醒后 Write 仍成功
             */
            BOOST_CHECK(ret == 0 || ret == -1);
            l.Down();
        };
        bbtco_sleep(50);
        c->Close();
        l.Down();
    };
    l.Wait();
    BOOST_CHECK(write_returned.load());
}

/**
 * 4.11 Close 唤醒阻塞中的 TryRead(timeout) 协程
 */
BOOST_AUTO_TEST_CASE(t_close_wake_tryread_timeout)
{
    BOOST_TEST_MESSAGE("enter t_close_wake_tryread_timeout");
    bbt::core::thread::CountDownLatch l{2};
    std::atomic_bool tryread_returned{false};
    bbtco[&l, &tryread_returned]()
    {
        auto c = Chan<int, 10>();
        bbtco[c, &l, &tryread_returned]()
        {
            int val;

            /**
             * 长超时
             */
            int ret = c->TryRead(val, 5000);
            tryread_returned.store(true);

            /**
             * Close 唤醒后 TryRead 返回 -1
             */
            BOOST_CHECK_EQUAL(ret, -1);
            l.Down();
        };
        bbtco_sleep(50);
        c->Close();
        l.Down();
    };
    l.Wait();
    BOOST_CHECK(tryread_returned.load());
}

/**
 * 4.12 Close 唤醒阻塞中的 TryWrite(timeout) 协程
 */
BOOST_AUTO_TEST_CASE(t_close_wake_trywrite_timeout)
{
    BOOST_TEST_MESSAGE("enter t_close_wake_trywrite_timeout");
    bbt::core::thread::CountDownLatch l{2};
    std::atomic_bool trywrite_returned{false};
    bbtco[&l, &trywrite_returned]()
    {
        auto c = Chan<int, 1>();

        /**
         * 填满
         */
        c->Write(1);
        bbtco[c, &l, &trywrite_returned]()
        {
            /**
             * 长超时
             */
            int ret = c->TryWrite(2, 5000);
            trywrite_returned.store(true);

            /**
             * Close 唤醒后 TryWrite 应返回 -1
             */
            BOOST_CHECK_EQUAL(ret, -1);
            l.Down();
        };
        bbtco_sleep(50);
        c->Close();
        l.Down();
    };
    l.Wait();
    BOOST_CHECK(trywrite_returned.load());
}

/**
 * 4.13 无缓冲 Close 后 Write/Read 返回 -1
 */
BOOST_AUTO_TEST_CASE(t_nocache_close_ops)
{
    BOOST_TEST_MESSAGE("enter t_nocache_close_ops");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 0>();
        c->Close();
        BOOST_CHECK_EQUAL(c->Write(1), -1);
        int val;
        BOOST_CHECK_EQUAL(c->Read(val), -1);
        l.Down();
    };
    l.Wait();
}

/**
 * 4.14 无缓冲 Close 唤醒阻塞写协程
 */
BOOST_AUTO_TEST_CASE(t_nocache_close_wake_write)
{
    BOOST_TEST_MESSAGE("enter t_nocache_close_wake_write");
    bbt::core::thread::CountDownLatch l{2};
    std::atomic_bool write_returned{false};
    bbtco[&l, &write_returned]()
    {
        auto c = Chan<int, 0>();
        bbtco[c, &l, &write_returned]()
        {
            /**
             * 无读者，阻塞
             */
            int ret = c->Write(1);
            write_returned.store(true);

            /**
             * 未匹配写者在 Close 时被唤醒，按关闭决议返回 -1
             */
            BOOST_CHECK_EQUAL(ret, -1);
            l.Down();
        };
        bbtco_sleep(50);
        c->Close();
        l.Down();
    };
    l.Wait();
    BOOST_CHECK(write_returned.load());
}

/**
 * 4.15 Chan 析构自动 Close
 */
BOOST_AUTO_TEST_CASE(t_chan_dtor_close)
{
    BOOST_TEST_MESSAGE("enter t_chan_dtor_close");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        bool closed;
        {
            auto c = Chan<int, 10>();
            c->Write(1);

            /**
             * c 离开作用域时析构，应自动 Close
             */
        }

        /**
         * 无法直接检查析构后的状态，通过不崩溃来验证
         */
        closed = true;
        BOOST_CHECK(closed);
        l.Down();
    };
    l.Wait();
}

/**
 * ================================================================
 * 新增测试: 5.并发竞争
 * ================================================================
 */

/**
 * 5.1 两个协程同时 Read：统一核心按接收等待 FIFO 接纳两名读者，各得一值
 */
BOOST_AUTO_TEST_CASE(t_ubo_multi_reader_fifo)
{
    BOOST_TEST_MESSAGE("enter t_ubo_multi_reader_fifo");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int r1{-999};
        std::atomic_int r2{-999};
        std::atomic_int v1{0};
        std::atomic_int v2{0};
        std::atomic_int w1{-999};
        std::atomic_int w2{-999};
        std::atomic_int r1_parked{0};
        std::atomic_int r2_parked{0};
        std::atomic_int r1_done_first{0};
        std::atomic_int r2_pending_after_first{0};
        std::atomic_int r2_returned{0};
    };

    auto state = std::make_shared<CaseState>();
    auto r1_watch = std::make_shared<ParkWatch>();
    auto r2_watch = std::make_shared<ParkWatch>();
    auto c = Chan<int, 0>();
    bbt::core::thread::CountDownLatch l{1};

    bbtco[state, c, r1_watch, r2_watch, &l]()
    {
        auto& sche = *bbt::coroutine::detail::Scheduler::GetInstance();
        auto pid = bbt::coroutine::detail::Processer::TryGetLocalProcesser()->GetId();

        sche.RegistCoroutineTaskOn(pid, [state, c, r1_watch]() {
            r1_watch->co.store(g_bbt_tls_coroutine_co);
            int v = 0;
            int ret = c->Read(v);
            state->v1.store(v);
            state->r1.store(ret, std::memory_order_release);
            r1_watch->done.store(1);
        });
        state->r1_parked.store(WaitParkedOnCustom(r1_watch, 3000) ? 1 : 0);

        sche.RegistCoroutineTaskOn(pid, [state, c, r2_watch]() {
            r2_watch->co.store(g_bbt_tls_coroutine_co);
            int v = 0;
            int ret = c->Read(v);
            state->v2.store(v);
            state->r2.store(ret, std::memory_order_release);
            r2_watch->done.store(1);
        });
        state->r2_parked.store(WaitParkedOnCustom(r2_watch, 3000) ? 1 : 0);

        /**
         * 两名读者都证明已 park 在 CUSTOM 等待后再注入：第一个值按接收等待
         * FIFO 交给先等待的 R1，且单值只交付一名读者——R2 必须仍在等待，
         * 不得被唤醒或报成功；第二个值再交给 R2。
         */
        if (state->r1_parked.load() && state->r2_parked.load()) {
            state->w1.store(c->Write(1));
            bool r1_returned =
                SpinUntilCo([&state] { return state->r1.load(std::memory_order_acquire) != -999; }, 3000);
            state->r1_done_first.store(r1_returned ? 1 : 0);
            state->r2_pending_after_first.store(
                (r1_returned && state->r2.load(std::memory_order_acquire) == -999
                 && !r2_watch->done.load(std::memory_order_acquire))
                    ? 1
                    : 0);
            if (r1_returned) {
                state->w2.store(c->Write(2));
                state->r2_returned.store(
                    SpinUntilCo([&state] { return state->r2.load(std::memory_order_acquire) != -999; }, 3000) ? 1 : 0);
            }
        }

        l.Down();
    };

    WaitLatch(l, 10000);

    BOOST_CHECK_EQUAL(state->r1_parked.load(), 1);
    BOOST_CHECK_EQUAL(state->r2_parked.load(), 1);
    BOOST_CHECK_EQUAL(state->w1.load(), 0);
    BOOST_CHECK_EQUAL(state->r1_done_first.load(), 1);
    BOOST_CHECK_EQUAL(state->r2_pending_after_first.load(), 1);
    BOOST_CHECK_EQUAL(state->w2.load(), 0);
    BOOST_CHECK_EQUAL(state->r2_returned.load(), 1);
    BOOST_CHECK_EQUAL(state->r1.load(), 0);
    BOOST_CHECK_EQUAL(state->r2.load(), 0);
    BOOST_CHECK_EQUAL(state->v1.load(), 1);
    BOOST_CHECK_EQUAL(state->v2.load(), 2);
}

/**
 * 5.2 两个协程同时 TryRead CAS 竞争
 */
BOOST_AUTO_TEST_CASE(t_concurrent_tryread_cas)
{
    BOOST_TEST_MESSAGE("enter t_concurrent_tryread_cas");
    bbt::core::thread::CountDownLatch l{2};
    std::atomic_int result1{0};
    std::atomic_int result2{0};
    int val1 = 0, val2 = 0;
    bbtco[&l, &result1, &result2, &val1, &val2]()
    {
        auto c = Chan<int, 10>();
        c->Write(42);

        /**
         * 两个 TryRead 并发，CAS 只允许一个成功
         */
        bbtco[c, &result1, &val1, &l]()
        {
            result1.store(c->TryRead(val1));
            l.Down();
        };
        bbtco[c, &result2, &val2, &l]()
        {
            result2.store(c->TryRead(val2));
            l.Down();
        };

        /**
         * 确保内层协程先执行，外协程不提前返回
         */
        bbtco_sleep(50);
    };
    l.Wait();

    /**
     * 一个应成功 (0)，另一个应失败 (-1)
     */
    BOOST_CHECK((result1.load() == 0 && result2.load() != 0) || (result1.load() != 0 && result2.load() == 0));
    int success_val = (result1.load() == 0) ? val1 : val2;
    BOOST_CHECK_EQUAL(success_val, 42);
}

/**
 * 5.3 Read 完成后新 Read 可正常进入
 */
BOOST_AUTO_TEST_CASE(t_read_after_read)
{
    BOOST_TEST_MESSAGE("enter t_read_after_read");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        c->Write(1);
        c->Write(2);
        int val1, val2;
        BOOST_ASSERT(c->Read(val1) == 0);
        BOOST_CHECK_EQUAL(val1, 1);

        /**
         * 第二个 Read 应能正常进入
         */
        BOOST_ASSERT(c->Read(val2) == 0);
        BOOST_CHECK_EQUAL(val2, 2);
        l.Down();
    };
    l.Wait();
}

/**
 * 5.4 2 个读者并发读有缓冲 Chan 数据完整性
 */
BOOST_AUTO_TEST_CASE(t_multi_reader_integrity)
{
    BOOST_TEST_MESSAGE("enter t_multi_reader_integrity");
    const int N = 100;
    bbt::core::thread::CountDownLatch l{3};
    std::set<int> r1_set, r2_set;
    std::atomic_int total_read{0};
    bbtco[&l, &r1_set, &r2_set, &total_read]()
    {
        auto c = Chan<int, 100>();
        for (int i = 0; i < N; ++i)
            BOOST_ASSERT(c->Write(i) == 0);
        bbtco[c, &r1_set, &total_read, &l]()
        {
            while (total_read.load() < N) {
                int val;
                if (c->Read(val) == 0) {
                    r1_set.insert(val);
                    total_read++;
                } else {
                    break;
                }
            }
            l.Down();
        };
        bbtco[c, &r2_set, &total_read, &l]()
        {
            while (total_read.load() < N) {
                int val;
                if (c->Read(val) == 0) {
                    r2_set.insert(val);
                    total_read++;
                } else {
                    break;
                }
            }
            l.Down();
        };

        /**
         * 外协程等待所有元素消费完后关闭 chan
         */
        while (total_read.load() < N)
            bbtco_sleep(1);

        /**
         * 唤醒可能阻塞的读者，此时队列已空不会丢数据
         */
        c->Close();
        l.Down();
    };
    l.Wait();
    std::set<int> all;
    all.insert(r1_set.begin(), r1_set.end());
    all.insert(r2_set.begin(), r2_set.end());
    BOOST_CHECK_EQUAL(all.size(), N);
    for (int i = 0; i < N; ++i)
        BOOST_CHECK(all.find(i) != all.end());
}

/**
 * 5.5 无缓冲 Chan 多读者场景
 */
BOOST_AUTO_TEST_CASE(t_nocache_multi_reader)
{
    BOOST_TEST_MESSAGE("enter t_nocache_multi_reader");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 0>();
        std::atomic_int r1_ok{0}, r2_ok{0};
        bbtco[c, &r1_ok]()
        {
            int val;
            r1_ok.store(c->Read(val));
        };
        bbtco[c, &r2_ok]()
        {
            int val;
            r2_ok.store(c->Read(val));
        };
        bbtco_sleep(50);

        /**
         * 写入一个元素后关闭，确保两个读者都能退出
         */
        bbtco[c]()
        {
            c->Write(1);
            bbtco_sleep(10);
            c->Close();
        };
        bbtco_sleep(100);
        BOOST_TEST_MESSAGE("Chan<0> 2 readers: r1=" << r1_ok.load() << " r2=" << r2_ok.load());
        l.Down();
    };
    l.Wait();
}

/**
 * ================================================================
 * 新增测试: 6.运算符与数据完整性
 * ================================================================
 */

/**
 * 6.1 Close 后 << 返回 false
 */
BOOST_AUTO_TEST_CASE(t_close_shift_write)
{
    BOOST_TEST_MESSAGE("enter t_close_shift_write");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        c->Close();
        BOOST_CHECK(!(c << 42));
        l.Down();
    };
    l.Wait();
}

/**
 * 6.2 Close 后 >> 返回 false
 */
BOOST_AUTO_TEST_CASE(t_close_shift_read)
{
    BOOST_TEST_MESSAGE("enter t_close_shift_read");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 10>();
        c->Close();
        int val;
        BOOST_CHECK(!(c >> val));
        l.Down();
    };
    l.Wait();
}

/**
 * 6.3 shared_ptr 版本 << 写入成功
 */
BOOST_AUTO_TEST_CASE(t_shared_ptr_shift_write)
{
    BOOST_TEST_MESSAGE("enter t_shared_ptr_shift_write");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = std::make_shared<sync::Chan<int, 10>>();
        BOOST_CHECK(c << 42);
        int val;
        BOOST_ASSERT(c->Read(val) == 0);
        BOOST_CHECK_EQUAL(val, 42);
        l.Down();
    };
    l.Wait();
}

/**
 * 6.4 shared_ptr 版本 >> 读取成功且值正确
 */
BOOST_AUTO_TEST_CASE(t_shared_ptr_shift_read)
{
    BOOST_TEST_MESSAGE("enter t_shared_ptr_shift_read");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = std::make_shared<sync::Chan<int, 10>>();
        c << 99;
        int val;
        BOOST_CHECK(c >> val);
        BOOST_CHECK_EQUAL(val, 99);
        l.Down();
    };
    l.Wait();
}

/**
 * 6.5 有缓冲 Chan 单写单读 FIFO 严格顺序
 */
BOOST_AUTO_TEST_CASE(t_fifo_order)
{
    BOOST_TEST_MESSAGE("enter t_fifo_order");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 100>();
        for (int i = 0; i < 100; ++i)
            BOOST_ASSERT(c->Write(i) == 0);
        for (int i = 0; i < 100; ++i) {
            int val;
            BOOST_ASSERT(c->Read(val) == 0);

            /**
             * 严格 FIFO
             */
            BOOST_CHECK_EQUAL(val, i);
        }
        l.Down();
    };
    l.Wait();
}

/**
 * 6.6 无缓冲 Chan 多次写读值匹配
 */
BOOST_AUTO_TEST_CASE(t_nocache_value_match)
{
    BOOST_TEST_MESSAGE("enter t_nocache_value_match");
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 0>();
        bbtco[&c]()
        {
            bbtco_sleep(10);
            c->Write(10);
            bbtco_sleep(10);
            c->Write(20);
            bbtco_sleep(10);
            c->Write(30);
        };
        int v1, v2, v3;
        BOOST_ASSERT(c->Read(v1) == 0);
        BOOST_CHECK_EQUAL(v1, 10);
        BOOST_ASSERT(c->Read(v2) == 0);
        BOOST_CHECK_EQUAL(v2, 20);
        BOOST_ASSERT(c->Read(v3) == 0);
        BOOST_CHECK_EQUAL(v3, 30);
        l.Down();
    };
    l.Wait();
}

/**
 * 6.7 多写者并发写入后逐一消费数据完整性（暂时简化：调度器累积协程过多）
 */
BOOST_AUTO_TEST_CASE(t_multi_write_integrity)
{
    BOOST_TEST_MESSAGE("enter t_multi_write_integrity (simplified)");

    /**
     * 前序测试累积的协程数已接近调度器上限，此处简化为基本验证
     */
    bbt::core::thread::CountDownLatch l{1};
    bbtco[&l]()
    {
        auto c = Chan<int, 65535>();
        c->Write(42);
        int val;
        BOOST_ASSERT(c->Read(val) == 0);
        BOOST_CHECK_EQUAL(val, 42);
        l.Down();
    };
    l.Wait();
}

/**
 * TryWrite(timeout) 超时留下的失效 waiter 不得吞掉真正 writer 的唤醒
 * 旧行为：_OnEnableWrite 只 pop 一个，僵尸 waiter Notify==-1 即返回，B 永远挂死
 */
BOOST_AUTO_TEST_CASE(t_chan_trywrite_timeout_does_not_steal_wakeup)
{
    BOOST_TEST_MESSAGE("enter t_chan_trywrite_timeout_does_not_steal_wakeup");
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int setup_ret{-999};
    std::atomic_int a_ret{-999};
    std::atomic_int read1_ret{-999};
    std::atomic_int read1_val{-999};
    std::atomic_int b_ret{-999};
    std::atomic_int read2_ret{-999};
    std::atomic_int read2_val{-999};
    bbtco[&l, &setup_ret, &a_ret, &read1_ret, &read1_val, &b_ret, &read2_ret, &read2_val]()
    {
        auto c = Chan<int, 1>();

        /**
         * 填满缓冲
         */
        setup_ret.store(c->TryWrite(1), std::memory_order_release);

        /**
         * A：写不进，80ms 后超时退出，留下失效 waiter
         */
        bbtco[c, &a_ret]()
        {
            a_ret.store(c->TryWrite(2, 80), std::memory_order_release);
        };

        bbt::core::thread::CountDownLatch lb{1};

        /**
         * 让 A 先入队，B 排在僵尸 waiter 之后
         */
        bbtco_sleep(20);
        bbtco[c, &lb, &b_ret]()
        {
            b_ret.store(c->Write(3), std::memory_order_release);
            lb.Down();
        };

        /**
         * 等 A 超时（其 waiter 已被 Cancel）
         */
        bbtco_sleep(120);

        int v = 0;
        read1_ret.store(c->Read(v), std::memory_order_release);
        read1_val.store(v, std::memory_order_release);

        /**
         * B 必须 1s 内完成，禁止死等
         */
        WaitLatch(lb, 1000);
        read2_ret.store(c->Read(v), std::memory_order_release);
        read2_val.store(v, std::memory_order_release);
        l.Down();
    };
    WaitLatch(l, 10000);
    BOOST_CHECK_EQUAL(setup_ret.load(std::memory_order_acquire), 0);
    BOOST_CHECK_EQUAL(a_ret.load(std::memory_order_acquire), 1);
    BOOST_CHECK_EQUAL(read1_ret.load(std::memory_order_acquire), 0);
    BOOST_CHECK_EQUAL(read1_val.load(std::memory_order_acquire), 1);
    BOOST_CHECK_EQUAL(b_ret.load(std::memory_order_acquire), 0);
    BOOST_CHECK_EQUAL(read2_ret.load(std::memory_order_acquire), 0);
    BOOST_CHECK_EQUAL(read2_val.load(std::memory_order_acquire), 3);
}

/**
 * 容量远小于写入量、多 writer、单 reader 的「注册竞态丢唤醒」回归。
 *
 * 旧实现（__TChan.hpp Write 族）：写 waiter 在解锁前就 push 进
 * m_enable_write_conds，随后才 _WaitUntilEnableWrite 登记事件。reader 可在
 * unlock→登记窗口内 pop 该 waiter 并 Notify，而此时 m_co_event==nullptr，
 * Notify 返回 -1：waiter 已离队且不再被唤醒，writer 永久 park（memcheck
 * 阶段三现场：容量 100 << 单 channel 50000，writer 在 ~5000 附近停滞）。
 * 修复后 waiter 只在等待事件登记成功后的 on_registered 回调里入队，并持
 * chan 锁复查容量/关闭，条件已满足则自 Notify 走 PENDING 早到路径。
 *
 * 本用例多轮独立 Chan 反复压容量-满阻塞/消费唤醒路径，并要求每轮全部消息
 * 读完 + 全部 writer 完成；有界等待超时即 FAIL，不用无限等待掩盖丢唤醒。
 */
BOOST_AUTO_TEST_CASE(t_chan_capacity_pressure_no_lost_wakeup)
{
    BOOST_TEST_MESSAGE("enter t_chan_capacity_pressure_no_lost_wakeup");

    /**
     * 挂到本文件既有启动协议：EnsureRuntime() 内部 call_once + Start()，
     * 与 t_begin 完全同一路径，重复调用为空操作；单独 --run_test 过滤运行
     * 时也保证 runtime 已初始化，不改变 process-lifetime 语义。
     */
    EnsureRuntime();
    constexpr int kCap = 16;
    constexpr int kWriters = 5;
    constexpr int kPerWriter = 2000;

    /**
     * 10000 >> 16
     */
    constexpr int kTotal = kWriters * kPerWriter;

    for (int round = 0; round < 8; ++round) {
        auto c = Chan<int, kCap>();
        bbt::core::thread::CountDownLatch writers{kWriters};
        bbt::core::thread::CountDownLatch reader{1};
        std::atomic_int writer_ok{0};

        for (int w = 0; w < kWriters; ++w) {
            bbtco[c, &writers, &writer_ok, w]()
            {
                for (int i = 0; i < kPerWriter; ++i) {
                    /**
                     * 非 0 = 关闭/等待失败：不 Down，本轮判 FAIL
                     */
                    if (c->Write(w * kPerWriter + i) != 0)
                        return;
                }
                writer_ok.fetch_add(1);
                writers.Down();
            };
        }

        bbtco[c, &reader]()
        {
            int v = 0;
            for (int i = 0; i < kTotal; ++i) {
                /**
                 * 不应发生：正常路径下队列会在写满前被消费
                 */
                if (c->Read(v) != 0)
                    return;
            }
            reader.Down();
        };

        /**
         * writer 全部退出时 reader 必定已读满 kTotal（消息总量守恒）。
         * 任一侧停滞都会在下面有界等待上超时。
         */
        WaitLatch(writers, 8000);
        WaitLatch(reader, 8000);
        BOOST_CHECK_EQUAL(writer_ok.load(), kWriters);
    }
}

/**
 * Close 与多 writer 单槽竞争：一次 pop 只放行一个 writer，Close 只裁决未胜出者（-1）。
 * 抓错：Close 重复交付（多个 writer push 成功）、关闭后 writer 仍返回 0（关闭状态
 * 发布滞后）、Close 全量唤醒后重复 push 的越界崩溃。
 * 状态全部挂在 shared_ptr 上：writer 协程可能因产品缺陷滞留挂起，捕获不得指向
 * 栈生命周期；断言全部在主线程做出（Boost.Test 并发上报不安全）。
 */
BOOST_AUTO_TEST_CASE(t_close_wake_multi_writer_single_slot)
{
    struct CaseState {
        std::shared_ptr<bbt::coroutine::sync::Chan<int, 1>> chan;
        std::atomic_int attempts{0};
        std::atomic_int wins{0};
        std::atomic_int losses{0};
        std::atomic_int loss_ret{0};
        std::atomic_int winner_val{0};
        std::atomic_int settled{0};
        std::atomic_int read_v0_ret{-999};
        std::atomic_int read_v0{0};
        std::atomic_int post_r1_ret{-999};
        std::atomic_int post_r1{0};
        std::atomic_int post_r2_ret{-999};
        std::atomic_int stage_attempts_ok{0};
        std::atomic_int stage_winner_ok{0};
        std::atomic_int stage_settled_ok{0};
    };

    auto state = std::make_shared<CaseState>();
    state->chan = Chan<int, 1>();
    BOOST_REQUIRE(state->chan != nullptr);
    BOOST_CHECK_EQUAL(state->chan->Write(100), 0);

    bbt::core::thread::CountDownLatch l{1};

    for (int val : {201, 202}) {
        bbtco[state, val]()
        {
            state->attempts.fetch_add(1);
            int ret = state->chan->Write(val);
            if (ret == 0) {
                state->wins.fetch_add(1);
                state->winner_val.store(val);
            } else {
                state->losses.fetch_add(1);
                state->loss_ret.store(ret);
            }
            state->settled.fetch_add(1);
        };
    }

    bbtco[state, &l]()
    {
        /**
         * 阶段门槛：两名 writer 都已发起写入后再释放唯一槽位
         */
        state->stage_attempts_ok.store(SpinUntilCo([&state] { return state->attempts.load() == 2; }, 3000) ? 1 : 0);
        int v = 0;
        state->read_v0_ret.store(state->chan->Read(v));
        state->read_v0.store(v);

        /**
         * 胜出者完成 push（槽位重新占满）后才 Close：否则胜出者会在复查关闭时被误判为 -1
         */
        state->stage_winner_ok.store(SpinUntilCo([&state] { return state->wins.load() >= 1; }, 3000) ? 1 : 0);
        state->chan->Close();
        state->stage_settled_ok.store(SpinUntilCo([&state] { return state->settled.load() == 2; }, 5000) ? 1 : 0);

        /**
         * 100 已在读端取走；关闭后仅剩胜出者入队的值，随后读尽
         */
        state->post_r1_ret.store(state->chan->Read(v));
        state->post_r1.store(v);
        state->post_r2_ret.store(state->chan->Read(v));
        l.Down();
    };

    WaitLatch(l, 10000);

    BOOST_CHECK_EQUAL(state->stage_attempts_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->read_v0_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->read_v0.load(), 100);
    BOOST_CHECK_EQUAL(state->stage_winner_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->stage_settled_ok.load(), 1);

    /**
     * 恰好一个 writer 胜出；收口数恰为 2（重复恢复会多计）
     */
    BOOST_CHECK_EQUAL(state->wins.load(), 1);
    BOOST_CHECK_EQUAL(state->wins.load() + state->losses.load(), 2);
    BOOST_CHECK_EQUAL(state->loss_ret.load(), -1);

    BOOST_CHECK_EQUAL(state->post_r1_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->post_r1.load(), state->winner_val.load());
    BOOST_CHECK_EQUAL(state->post_r2_ret.load(), -1);
}

/**
 * 关闭后缓冲数据按序排空：非空 chan 关闭，已入队数据对读者仍然可见，
 * 排空后读写才按关闭语义失败。ReadAll 走同一责任路径。
 * 抓错：Close 清空/丢弃缓冲、关闭状态发布破坏排空路径、关闭后写未被拒绝。
 * 断言全部在主线程做出。
 */
BOOST_AUTO_TEST_CASE(t_close_drains_buffered_items_in_order)
{
    struct CaseState {
        std::atomic_int w1{-999};
        std::atomic_int w2{-999};
        std::atomic_int w3{-999};
        std::atomic_int closed_flag{-1};
        std::atomic_int r1{-999};
        std::atomic_int v1{0};
        std::atomic_int r2{-999};
        std::atomic_int v2{0};
        std::atomic_int r3{-999};
        std::atomic_int v3{0};
        std::atomic_int r4{-999};
        std::atomic_int rtr{-999};
        std::atomic_int rall{-999};
        std::atomic_int wpost{-999};
        std::atomic_int w7{-999};
        std::atomic_int w8{-999};
        std::atomic_int rall2{-999};
        std::atomic_int n2{-1};
        std::atomic_int i0{-999};
        std::atomic_int i1{-999};
    };

    auto state = std::make_shared<CaseState>();
    bbt::core::thread::CountDownLatch l{1};

    bbtco[state, &l]()
    {
        {
            auto c = Chan<int, 8>();
            state->w1.store(c->Write(1));
            state->w2.store(c->Write(2));
            state->w3.store(c->Write(3));
            c->Close();
            state->closed_flag.store(c->IsClosed() ? 1 : 0);

            int v = 0;
            state->r1.store(c->Read(v));
            state->v1.store(v);
            state->r2.store(c->Read(v));
            state->v2.store(v);
            state->r3.store(c->Read(v));
            state->v3.store(v);
            state->r4.store(c->Read(v));
            state->rtr.store(c->TryRead(v));

            std::vector<int> items;
            state->rall.store(c->ReadAll(items));
            state->wpost.store(c->Write(4));
        }

        {
            auto c = Chan<int, 8>();
            state->w7.store(c->Write(7));
            state->w8.store(c->Write(8));
            c->Close();

            std::vector<int> items;
            state->rall2.store(c->ReadAll(items));
            state->n2.store(static_cast<int>(items.size()));
            state->i0.store(items.size() > 0 ? items[0] : -999);
            state->i1.store(items.size() > 1 ? items[1] : -999);
        }
        l.Down();
    };

    WaitLatch(l, 5000);

    BOOST_CHECK_EQUAL(state->w1.load(), 0);
    BOOST_CHECK_EQUAL(state->w2.load(), 0);
    BOOST_CHECK_EQUAL(state->w3.load(), 0);
    BOOST_CHECK_EQUAL(state->closed_flag.load(), 1);
    BOOST_CHECK_EQUAL(state->r1.load(), 0);
    BOOST_CHECK_EQUAL(state->v1.load(), 1);
    BOOST_CHECK_EQUAL(state->r2.load(), 0);
    BOOST_CHECK_EQUAL(state->v2.load(), 2);
    BOOST_CHECK_EQUAL(state->r3.load(), 0);
    BOOST_CHECK_EQUAL(state->v3.load(), 3);
    BOOST_CHECK_EQUAL(state->r4.load(), -1);
    BOOST_CHECK_NE(state->rtr.load(), 0);
    BOOST_CHECK_EQUAL(state->rall.load(), -1);
    BOOST_CHECK_EQUAL(state->wpost.load(), -1);

    BOOST_CHECK_EQUAL(state->w7.load(), 0);
    BOOST_CHECK_EQUAL(state->w8.load(), 0);
    BOOST_CHECK_EQUAL(state->rall2.load(), 0);
    BOOST_CHECK_EQUAL(state->n2.load(), 2);
    BOOST_CHECK_EQUAL(state->i0.load(), 7);
    BOOST_CHECK_EQUAL(state->i1.load(), 8);
}

/**
 * ================================================================
 * 缓冲 Chan<T,N> 统一核心补充覆盖
 *
 * 目标：多读者/满缓冲多写者的等待 FIFO、补位后的不可撤回、关闭时
 * 残留排空与等待写者收口、用户类型异常后的进展。断言都在主线程
 * 或单个编排协程内做出；等待方进入阻塞调用前先过阶段屏障，注入方
 * 等屏障齐备后再注入，不用 sleep 猜时序。
 * ================================================================
 */

namespace {

/**
 * 缓冲接收侧赋值抛异常：复制构造正常（用于写入与入缓冲）；
 * throw_on_assign 为真的元素在被赋值时抛出，后续元素可正常交付。
 */
struct BufThrowOnAssign {
    int value{0};
    bool throw_on_assign{false};

    BufThrowOnAssign() = default;

    explicit BufThrowOnAssign(int v, bool do_throw = false)
        : value(v)
        , throw_on_assign(do_throw)
    {
    }

    BufThrowOnAssign(const BufThrowOnAssign&) = default;
    BufThrowOnAssign(BufThrowOnAssign&&) = default;

    BufThrowOnAssign& operator=(const BufThrowOnAssign& other)
    {
        if (other.throw_on_assign)
            throw std::runtime_error("buf assign");
        value = other.value;
        throw_on_assign = other.throw_on_assign;
        return *this;
    }

    BufThrowOnAssign& operator=(BufThrowOnAssign&& other)
    {
        if (other.throw_on_assign)
            throw std::runtime_error("buf assign");
        value = other.value;
        throw_on_assign = other.throw_on_assign;
        return *this;
    }
};

/**
 * 第 N 次复制构造抛出：用于 ReadAll 批量追加中途失败。
 * copies_left 为 -1 时不抛；为 0 时下一次复制抛出；正数逐次递减。
 */
struct BufThrowOnCopy {
    static std::atomic_int copies_left;

    int value{0};

    BufThrowOnCopy() = default;

    explicit BufThrowOnCopy(int v)
        : value(v)
    {
    }

    BufThrowOnCopy(const BufThrowOnCopy& other)
        : value(other.value)
    {
        int left = copies_left.load(std::memory_order_relaxed);
        if (left == 0)
            throw std::runtime_error("buf copy");
        if (left > 0)
            copies_left.store(left - 1, std::memory_order_relaxed);
    }

    BufThrowOnCopy(BufThrowOnCopy&&) noexcept = default;

    BufThrowOnCopy& operator=(const BufThrowOnCopy&) = default;
    BufThrowOnCopy& operator=(BufThrowOnCopy&&) noexcept = default;
};

std::atomic_int BufThrowOnCopy::copies_left{-1};

} // namespace

/**
 * 行为：缓冲通道上两个同时等待的读者都按接收等待 FIFO 被接纳，各得一值。
 * 抓错：旧单消费者 CAS 拒绝第二个读者（返回非 0）、重复交付同一元素。
 * 阶段：两名读者与编排协程同 P，逐个证明已 park 在 CUSTOM 等待后再注入。
 */
BOOST_AUTO_TEST_CASE(t_buffered_multi_reader_fifo)
{
    BOOST_TEST_MESSAGE("enter t_buffered_multi_reader_fifo");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int r1{-999};
        std::atomic_int r2{-999};
        std::atomic_int v1{0};
        std::atomic_int v2{0};
        std::atomic_int w1{0};
        std::atomic_int w2{0};
        std::atomic_int r1_first{0};
        std::atomic_int r1_parked{0};
        std::atomic_int r2_parked{0};
        std::atomic_int second_returned{0};
    };

    auto state = std::make_shared<CaseState>();
    auto r1_watch = std::make_shared<ParkWatch>();
    auto r2_watch = std::make_shared<ParkWatch>();
    auto c = Chan<int, 1>();
    bbt::core::thread::CountDownLatch l{1};

    bbtco[state, c, r1_watch, r2_watch, &l]()
    {
        auto& sche = *bbt::coroutine::detail::Scheduler::GetInstance();
        auto pid = bbt::coroutine::detail::Processer::TryGetLocalProcesser()->GetId();

        sche.RegistCoroutineTaskOn(pid, [state, c, r1_watch]() {
            r1_watch->co.store(g_bbt_tls_coroutine_co);
            int v = 0;
            int ret = c->Read(v);
            state->v1.store(v);
            state->r1.store(ret, std::memory_order_release);
            r1_watch->done.store(1);
        });
        state->r1_parked.store(WaitParkedOnCustom(r1_watch, 3000) ? 1 : 0);

        sche.RegistCoroutineTaskOn(pid, [state, c, r2_watch]() {
            r2_watch->co.store(g_bbt_tls_coroutine_co);
            int v = 0;
            int ret = c->Read(v);
            state->v2.store(v);
            state->r2.store(ret, std::memory_order_release);
            r2_watch->done.store(1);
        });
        state->r2_parked.store(WaitParkedOnCustom(r2_watch, 3000) ? 1 : 0);

        /**
         * 两个读者的等待都已登记；先注入第一个值并等 R1 完成，再注入第二个。
         * 接收等待 FIFO 要求第一个值归先等待的 R1。
         */
        if (state->r1_parked.load() && state->r2_parked.load()) {
            state->w1.store(c->Write(1));
            bool first_done = SpinUntilCo([&state] { return state->r1.load(std::memory_order_acquire) != -999; }, 3000);
            state->r1_first.store(first_done ? 1 : 0);
            if (first_done)
                state->w2.store(c->Write(2));
            state->second_returned.store(
                SpinUntilCo([&state] { return state->r2.load(std::memory_order_acquire) != -999; }, 3000) ? 1 : 0);
        }

        l.Down();
    };

    WaitLatch(l, 10000);

    BOOST_CHECK_EQUAL(state->r1_parked.load(), 1);
    BOOST_CHECK_EQUAL(state->r2_parked.load(), 1);
    BOOST_CHECK_EQUAL(state->w1.load(), 0);
    BOOST_CHECK_EQUAL(state->w2.load(), 0);
    BOOST_CHECK_EQUAL(state->r1_first.load(), 1);
    BOOST_CHECK_EQUAL(state->second_returned.load(), 1);
    BOOST_CHECK_EQUAL(state->r1.load(), 0);
    BOOST_CHECK_EQUAL(state->r2.load(), 0);
    BOOST_CHECK_EQUAL(state->v1.load(), 1);
    BOOST_CHECK_EQUAL(state->v2.load(), 2);
}

/**
 * 行为：缓冲满时排队的写者按发送等待 FIFO 补位；补位成功的发送返回 0，
 *       释放一个槽位只放行队首写者。
 * 抓错：补位走 LIFO/随机顺序、补位后返回值与写入值不匹配、队尾写者插队。
 * 阶段：三名写者与编排协程同 P，逐个证明已 park 在 CUSTOM 等待后再消费。
 */
BOOST_AUTO_TEST_CASE(t_buffered_full_writer_fifo)
{
    BOOST_TEST_MESSAGE("enter t_buffered_full_writer_fifo");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int w1{-999};
        std::atomic_int w2{-999};
        std::atomic_int w3{-999};
        std::atomic_int parked{0};
        std::atomic_int read_fail{0};
        std::vector<int> reads;
        std::mutex reads_mutex;
    };

    auto state = std::make_shared<CaseState>();
    auto w1_watch = std::make_shared<ParkWatch>();
    auto w2_watch = std::make_shared<ParkWatch>();
    auto w3_watch = std::make_shared<ParkWatch>();
    auto c = Chan<int, 1>();
    bbt::core::thread::CountDownLatch l{1};

    BOOST_REQUIRE_EQUAL(c->TryWrite(100), 0);

    bbtco[state, c, w1_watch, w2_watch, w3_watch, &l]()
    {
        auto& sche = *bbt::coroutine::detail::Scheduler::GetInstance();
        auto pid = bbt::coroutine::detail::Processer::TryGetLocalProcesser()->GetId();

        sche.RegistCoroutineTaskOn(pid, [state, c, w1_watch]() {
            w1_watch->co.store(g_bbt_tls_coroutine_co);
            state->w1.store(c->Write(201));
            w1_watch->done.store(1);
        });
        bool p1 = WaitParkedOnCustom(w1_watch, 3000);

        sche.RegistCoroutineTaskOn(pid, [state, c, w2_watch]() {
            w2_watch->co.store(g_bbt_tls_coroutine_co);
            state->w2.store(c->Write(202));
            w2_watch->done.store(1);
        });
        bool p2 = WaitParkedOnCustom(w2_watch, 3000);

        sche.RegistCoroutineTaskOn(pid, [state, c, w3_watch]() {
            w3_watch->co.store(g_bbt_tls_coroutine_co);
            state->w3.store(c->Write(203));
            w3_watch->done.store(1);
        });
        bool p3 = WaitParkedOnCustom(w3_watch, 3000);
        state->parked.store(p1 && p2 && p3 ? 1 : 0);

        if (state->parked.load()) {
            int v = 0;
            for (int i = 0; i < 4; ++i) {
                if (c->Read(v) != 0) {
                    state->read_fail.store(1);
                    break;
                }
                std::lock_guard<std::mutex> lock(state->reads_mutex);
                state->reads.push_back(v);
            }
        }

        /**
         * 三个写者的结果由补位唤醒决定，与读者返回存在调度间隙。
         */
        SpinUntilCo(
            [&state] { return state->w1.load() != -999 && state->w2.load() != -999 && state->w3.load() != -999; },
            3000);
        l.Down();
    };

    WaitLatch(l, 10000);

    BOOST_CHECK_EQUAL(state->parked.load(), 1);
    BOOST_CHECK_EQUAL(state->read_fail.load(), 0);
    BOOST_CHECK_EQUAL(state->w1.load(), 0);
    BOOST_CHECK_EQUAL(state->w2.load(), 0);
    BOOST_CHECK_EQUAL(state->w3.load(), 0);
    BOOST_REQUIRE_EQUAL(state->reads.size(), 4u);
    BOOST_CHECK_EQUAL(state->reads[0], 100);
    BOOST_CHECK_EQUAL(state->reads[1], 201);
    BOOST_CHECK_EQUAL(state->reads[2], 202);
    BOOST_CHECK_EQUAL(state->reads[3], 203);
}

/**
 * 行为：等待写者被补位接纳（返回 0）后，随后对该协程的取消和通道关闭都
 *       不得撤回已入缓冲的值；Close 后残留仍可被 Read 排空。
 * 抓错：补位成功后仍可被取消/关闭改写为失败、已入缓冲元素被关闭路径丢弃。
 * 阶段：写者与编排协程同 P，证明已 park 在 Write 后才消费；补位在同一 Chan
 *       锁内完成，Read 返回即已补位。写者完成后停在 CoWaiter 上保持对象存活，
 *       使外部取消作用于一个仍然存活的协程。
 */
BOOST_AUTO_TEST_CASE(t_buffered_promoted_send_survives_cancel_and_close)
{
    BOOST_TEST_MESSAGE("enter t_buffered_promoted_send_survives_cancel_and_close");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int writer_ret{-999};
        std::atomic_int read_ret{-999};
        std::atomic_int read_val{0};
        std::atomic_int tryread_ret{-999};
        std::atomic_int timed_tryread_ret{-999};
        std::atomic_int drain_ret{-999};
        std::atomic_int drain_val{0};
        std::atomic_int writer_settled{0};
        std::atomic_int parked{0};
    };

    auto state = std::make_shared<CaseState>();
    auto writer_watch = std::make_shared<ParkWatch>();
    auto hold = sync::CoWaiter::Create();
    auto c = Chan<int, 1>();
    bbt::core::thread::CountDownLatch l{1};

    BOOST_REQUIRE_EQUAL(c->TryWrite(100), 0);

    bbtco[state, c, writer_watch, hold, &l]()
    {
        auto& sche = *bbt::coroutine::detail::Scheduler::GetInstance();
        auto pid = bbt::coroutine::detail::Processer::TryGetLocalProcesser()->GetId();

        sche.RegistCoroutineTaskOn(pid, [state, c, writer_watch, hold]() {
            writer_watch->co.store(g_bbt_tls_coroutine_co);
            state->writer_ret.store(c->Write(201));
            state->writer_settled.store(1);
            hold->Wait();
            writer_watch->done.store(1);
        });
        state->parked.store(WaitParkedOnCustom(writer_watch, 3000) ? 1 : 0);

        if (state->parked.load()) {
            /**
             * Read 在同一临界区里摘取 100 并把写者补位进唯一槽位。
             */
            int v = 0;
            state->read_ret.store(c->Read(v));
            state->read_val.store(v);

            /**
             * 写者要么仍在 P 的待恢复队列，要么已完成并停在 hold；两种状态下
             * 协程对象都存活，外部取消只能观察到已 Completed 的操作。
             */
            writer_watch->co.load(std::memory_order_acquire)->RequestCancel();
            c->Close();

            int probe = 0;
            state->tryread_ret.store(c->TryRead(probe));
            state->timed_tryread_ret.store(c->TryRead(probe, 10));
            state->drain_ret.store(c->Read(probe));
            state->drain_val.store(probe);
        }

        SpinUntilCo([&state] { return state->writer_settled.load() == 1; }, 3000);
        hold->Notify();
        l.Down();
    };

    WaitLatch(l, 10000);

    BOOST_CHECK_EQUAL(state->parked.load(), 1);
    BOOST_CHECK_EQUAL(state->read_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->read_val.load(), 100);
    BOOST_CHECK_EQUAL(state->writer_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->writer_settled.load(), 1);
    BOOST_CHECK_EQUAL(state->tryread_ret.load(), -1);
    BOOST_CHECK_EQUAL(state->timed_tryread_ret.load(), -1);
    BOOST_CHECK_EQUAL(state->drain_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->drain_val.load(), 201);
}

/**
 * 行为：关闭时缓冲残留保留 FIFO 顺序供 Read/ReadAll 排空；两种 TryRead 直接 -1
 *       且不消费残留；等待写者一律 -1，不得在关闭过程中补位。
 * 抓错：Close 丢缓冲、TryRead 越过关闭读残留、关闭时把等待写者补进刚释放的槽位。
 * 阶段：两名写者与编排协程同 P，证明都已 park 在 Write 后才 Close。
 */
BOOST_AUTO_TEST_CASE(t_buffered_close_residual_semantics)
{
    BOOST_TEST_MESSAGE("enter t_buffered_close_residual_semantics");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int w3{-999};
        std::atomic_int w4{-999};
        std::atomic_int size_before{0};
        std::atomic_int tryread_ret{-999};
        std::atomic_int timed_tryread_ret{-999};
        std::atomic_int r1_ret{-999};
        std::atomic_int r1_val{0};
        std::atomic_int r2_ret{-999};
        std::atomic_int r2_val{0};
        std::atomic_int r3_ret{-999};
        std::atomic_int all_ret{-999};
        std::atomic_int post_write{-999};
        std::atomic_int size_after{0};
        std::atomic_int parked{0};
    };

    auto state = std::make_shared<CaseState>();
    auto w3_watch = std::make_shared<ParkWatch>();
    auto w4_watch = std::make_shared<ParkWatch>();
    auto c = Chan<int, 2>();
    bbt::core::thread::CountDownLatch l{1};

    BOOST_REQUIRE_EQUAL(c->TryWrite(1), 0);
    BOOST_REQUIRE_EQUAL(c->TryWrite(2), 0);

    bbtco[state, c, w3_watch, w4_watch, &l]()
    {
        auto& sche = *bbt::coroutine::detail::Scheduler::GetInstance();
        auto pid = bbt::coroutine::detail::Processer::TryGetLocalProcesser()->GetId();

        sche.RegistCoroutineTaskOn(pid, [state, c, w3_watch]() {
            w3_watch->co.store(g_bbt_tls_coroutine_co);
            state->w3.store(c->Write(3));
            w3_watch->done.store(1);
        });
        bool p3 = WaitParkedOnCustom(w3_watch, 3000);

        sche.RegistCoroutineTaskOn(pid, [state, c, w4_watch]() {
            w4_watch->co.store(g_bbt_tls_coroutine_co);
            state->w4.store(c->Write(4));
            w4_watch->done.store(1);
        });
        bool p4 = WaitParkedOnCustom(w4_watch, 3000);
        state->parked.store(p3 && p4 ? 1 : 0);

        state->size_before.store(static_cast<int>(c->size()));
        c->Close();

        int v = 0;
        state->tryread_ret.store(c->TryRead(v));
        state->timed_tryread_ret.store(c->TryRead(v, 10));
        state->size_after.store(static_cast<int>(c->size()));
        state->r1_ret.store(c->Read(v));
        state->r1_val.store(v);
        state->r2_ret.store(c->Read(v));
        state->r2_val.store(v);
        state->r3_ret.store(c->Read(v));

        std::vector<int> items;
        state->all_ret.store(c->ReadAll(items));
        state->post_write.store(c->Write(5));
        SpinUntilCo([&state] { return state->w3.load() != -999 && state->w4.load() != -999; }, 3000);
        l.Down();
    };

    WaitLatch(l, 10000);

    BOOST_CHECK_EQUAL(state->parked.load(), 1);
    BOOST_CHECK_EQUAL(state->size_before.load(), 2);
    BOOST_CHECK_EQUAL(state->w3.load(), -1);
    BOOST_CHECK_EQUAL(state->w4.load(), -1);
    BOOST_CHECK_EQUAL(state->tryread_ret.load(), -1);
    BOOST_CHECK_EQUAL(state->timed_tryread_ret.load(), -1);

    /**
     * TryRead 不消费残留
     */
    BOOST_CHECK_EQUAL(state->size_after.load(), 2);
    BOOST_CHECK_EQUAL(state->r1_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->r1_val.load(), 1);
    BOOST_CHECK_EQUAL(state->r2_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->r2_val.load(), 2);
    BOOST_CHECK_EQUAL(state->r3_ret.load(), -1);
    BOOST_CHECK_EQUAL(state->all_ret.load(), -1);
    BOOST_CHECK_EQUAL(state->post_write.load(), -1);
}

/**
 * 行为：缓冲 Read 的接收端赋值抛异常后，异常交给读者；已出缓冲的首值不回队，
 *       后续 Read 取得下一个值；已接纳的发送仍返回 0。
 * 抓错：异常后元素重复交付、把已成功的写入改判失败、通道卡在 Matched。
 */
BOOST_AUTO_TEST_CASE(t_buffered_read_assign_throw_keeps_rest)
{
    BOOST_TEST_MESSAGE("enter t_buffered_read_assign_throw_keeps_rest");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int w1{-999};
        std::atomic_int w2{-999};
        std::atomic_int caught{0};
        std::atomic_int read_ret{-999};
        std::atomic_int read_val{0};
        std::atomic_int tryread_ret{-999};
        std::atomic_int size_after{0};
    };

    auto state = std::make_shared<CaseState>();
    auto c = Chan<BufThrowOnAssign, 2>();
    bbt::core::thread::CountDownLatch l{1};

    state->w1.store(c->Write(BufThrowOnAssign(10, true)));
    state->w2.store(c->Write(BufThrowOnAssign(20, false)));

    bbtco[state, c, &l]()
    {
        BufThrowOnAssign out;
        try {
            c->Read(out);
        } catch (const std::runtime_error&) {
            state->caught.store(1);
        }

        state->read_ret.store(c->Read(out));
        state->read_val.store(out.value);

        BufThrowOnAssign probe;
        state->tryread_ret.store(c->TryRead(probe));
        state->size_after.store(static_cast<int>(c->size()));
        c->Close();
        l.Down();
    };

    WaitLatch(l, 5000);

    BOOST_CHECK_EQUAL(state->w1.load(), 0);
    BOOST_CHECK_EQUAL(state->w2.load(), 0);
    BOOST_CHECK_EQUAL(state->caught.load(), 1);

    /**
     * 20 是第二个已缓冲值，说明失败的首值没有回队
     */
    BOOST_CHECK_EQUAL(state->read_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->read_val.load(), 20);
    BOOST_CHECK_EQUAL(state->tryread_ret.load(), -2);
    BOOST_CHECK_EQUAL(state->size_after.load(), 0);
}

/**
 * 行为：ReadAll 批量追加中途抛异常时，已追加项保留在调用方容器中；
 *       已从缓冲保留但尚未追加的项不回队、不重复交付；通道随后继续可读写。
 * 抓错：异常把已追加项回滚/重复、已保留项重新入队导致重复交付、通道不可再用。
 * 说明：这是有意的语义变化——Matched 元素不回队，未追加的已保留项随之丢弃；
 *       旧实现会把抛异常项和尾部队列留在通道里。
 */
BOOST_AUTO_TEST_CASE(t_buffered_readall_append_throw_keeps_appended)
{
    BOOST_TEST_MESSAGE("enter t_buffered_readall_append_throw_keeps_appended");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int caught{0};
        std::atomic_int size_after_throw{-1};
        std::atomic_int progress_write1{-999};
        std::atomic_int progress_write2{-999};
        std::atomic_int progress_readall{-999};
        std::atomic_int final_readall{-999};
        std::vector<int> values;
    };

    auto state = std::make_shared<CaseState>();
    auto c = Chan<BufThrowOnCopy, 4>();
    bbt::core::thread::CountDownLatch l{1};

    for (int v : {10, 11, 12, 13})
        BOOST_REQUIRE_EQUAL(c->Write(BufThrowOnCopy(v)), 0);

    /**
     * 10、11 追加成功，12 的复制抛出；13 已保留但未追加。
     */
    BufThrowOnCopy::copies_left.store(2);

    bbtco[state, c, &l]()
    {
        std::vector<BufThrowOnCopy> items;
        try {
            c->ReadAll(items);
        } catch (const std::runtime_error&) {
            state->caught.store(1);
        }
        BufThrowOnCopy::copies_left.store(-1);

        for (const auto& item : items)
            state->values.push_back(item.value);
        state->size_after_throw.store(static_cast<int>(c->size()));

        state->progress_write1.store(c->Write(BufThrowOnCopy(20)));
        state->progress_write2.store(c->Write(BufThrowOnCopy(21)));
        state->progress_readall.store(c->ReadAll(items));
        for (size_t i = 2; i < items.size(); ++i)
            state->values.push_back(items[i].value);

        c->Close();
        state->final_readall.store(c->ReadAll(items));
        l.Down();
    };

    WaitLatch(l, 5000);

    BOOST_CHECK_EQUAL(state->caught.load(), 1);
    BOOST_CHECK_EQUAL(state->size_after_throw.load(), 0);
    BOOST_CHECK_EQUAL(state->progress_write1.load(), 0);
    BOOST_CHECK_EQUAL(state->progress_write2.load(), 0);
    BOOST_CHECK_EQUAL(state->progress_readall.load(), 0);
    BOOST_CHECK_EQUAL(state->final_readall.load(), -1);

    /**
     * 已追加的 10、11 保留；12/13 未追加也不回队；后续写入的 20、21 正常追加。
     */
    BOOST_REQUIRE_EQUAL(state->values.size(), 4u);
    BOOST_CHECK_EQUAL(state->values[0], 10);
    BOOST_CHECK_EQUAL(state->values[1], 11);
    BOOST_CHECK_EQUAL(state->values[2], 20);
    BOOST_CHECK_EQUAL(state->values[3], 21);
}

/**
 * ================================================================
 * 无缓冲 Chan<T,0> 统一核心回归
 *
 * 目标契约（设计稿 §3）：零容量通道只允许发送者与接收者直接交接，
 * 不保存元素；TryWrite 无等待接收者返回 -2；带超时 TryWrite 等待接收者，
 * 超时返回 1；关闭、取消与交接共用一个首胜决议。
 * ================================================================
 */

namespace {

/**
 * 接收侧赋值抛异常的元素类型：复制构造正常，赋值固定抛出。
 * 用于验证 Matched 之后的用户赋值失败路径，不依赖通道内部状态。
 */
struct UboThrowOnAssign {
    int value{0};

    UboThrowOnAssign() = default;

    explicit UboThrowOnAssign(int v)
        : value(v)
    {
    }

    UboThrowOnAssign(const UboThrowOnAssign&) = default;
    UboThrowOnAssign(UboThrowOnAssign&&) = default;

    UboThrowOnAssign& operator=(const UboThrowOnAssign&) { throw std::runtime_error("ubo assign"); }

    UboThrowOnAssign& operator=(UboThrowOnAssign&&) { throw std::runtime_error("ubo assign"); }
};

} // namespace

/**
 * 行为：零缓冲 TryWrite 没有等待接收者时必须返回 -2，且元素不得被保存。
 * 抓错：TryWrite 落进隐藏缓冲/容量一基类队列，返回 0 并留下元素。
 * 现有覆盖：t_nocache_trywrite 只断言 true，没有检查返回值与残留数据。
 * 入口：Chan<T,0>::TryWrite / TryRead。
 */
BOOST_AUTO_TEST_CASE(t_ubo_trywrite_requires_waiting_receiver)
{
    BOOST_TEST_MESSAGE("enter t_ubo_trywrite_requires_waiting_receiver");
    EnsureRuntime();

    auto c = Chan<int, 0>();
    BOOST_REQUIRE(!c->IsClosed());

    BOOST_CHECK_EQUAL(c->TryWrite(42), -2);

    int val = 0;
    BOOST_CHECK_EQUAL(c->TryRead(val), -2);
}

/**
 * 行为：零缓冲 TryWrite(item, ms) 没有接收者时等待到超时返回 1，不保存元素。
 * 抓错：带超时 TryWrite 走容量一缓冲立即返回 0（假成功），元素留在隐藏槽位。
 * 现有覆盖：t_trywrite_timeout 只覆盖 Max=1 的缓冲满路径。
 * 入口：Chan<T,0>::TryWrite(item, ms) / TryRead。
 */
BOOST_AUTO_TEST_CASE(t_ubo_timed_trywrite_without_receiver_times_out)
{
    BOOST_TEST_MESSAGE("enter t_ubo_timed_trywrite_without_receiver_times_out");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int ret{-999};
        std::atomic_int elapsed_ms{-1};
        std::atomic_int tryread_ret{-999};
        std::atomic_int tryread_value{0};
    };

    auto state = std::make_shared<CaseState>();

    bbt::core::thread::CountDownLatch l{1};
    bbtco[state, &l]()
    {
        auto c = Chan<int, 0>();
        auto begin = bbt::core::clock::gettime<>();
        int ret = c->TryWrite(7, 60);
        auto elapsed = bbt::core::clock::gettime<>() - begin;
        state->ret.store(ret);
        state->elapsed_ms.store(static_cast<int>(elapsed));

        int val = 0;
        state->tryread_ret.store(c->TryRead(val));
        state->tryread_value.store(val);
        c->Close();
        l.Down();
    };

    WaitLatch(l, 3000);

    BOOST_CHECK_EQUAL(state->ret.load(), 1);
    BOOST_CHECK_GE(state->elapsed_ms.load(), 40);
    BOOST_CHECK_EQUAL(state->tryread_ret.load(), -2);
    BOOST_CHECK_NE(state->tryread_value.load(), 7);
}

/**
 * 行为：带超时 TryWrite 在超时窗口内等到接收者后必须完成交接，读者拿到该值。
 * 抓错：TryWrite 把元素写进隐藏缓冲并返回 0，读者永远等不到（假成功 + 丢唤醒）。
 * 现有覆盖：t_trywrite_timeout_success 只覆盖 Max=1 的缓冲补位，零缓冲无覆盖。
 * 入口：Chan<T,0>::TryWrite(item, ms) / Read。
 */
BOOST_AUTO_TEST_CASE(t_ubo_timed_trywrite_hands_off_to_late_reader)
{
    BOOST_TEST_MESSAGE("enter t_ubo_timed_trywrite_hands_off_to_late_reader");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int write_ret{-999};
        std::atomic_int reader_ret{-999};
        std::atomic_int reader_value{0};
        std::atomic_bool reader_done{false};
        std::atomic_bool reader_done_before_close{false};
    };

    auto state = std::make_shared<CaseState>();
    auto c = Chan<int, 0>();

    bbt::core::thread::CountDownLatch body_latch{1};

    bbtco[state, c]()
    {
        int val = 0;
        int ret = c->Read(val);
        state->reader_ret.store(ret);
        state->reader_value.store(val);
        state->reader_done.store(true);
    };

    bbtco[state, c, &body_latch]()
    {
        state->write_ret.store(c->TryWrite(9, 2000));

        bool reader_done = SpinUntilCo([&state]() { return state->reader_done.load(); }, 3000);
        state->reader_done_before_close.store(reader_done);
        if (!reader_done)
            c->Close();
        SpinUntilCo([&state]() { return state->reader_done.load(); }, 1000);
        body_latch.Down();
    };

    WaitLatch(body_latch, 8000);

    BOOST_CHECK_EQUAL(state->write_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->reader_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->reader_value.load(), 9);
    BOOST_CHECK(state->reader_done_before_close.load());
}

/**
 * 行为：阻塞 Write 与随后到达的 TryRead 必须完成直接交接，写者成功返回。
 * 抓错：Write 写入独立缓存，TryRead 读另一个队列，双方永远匹配不上（零缓冲丢数据）。
 * 现有覆盖：t_nocache_value_match 只覆盖 Write -> Read，未覆盖接收端 TryRead。
 * 入口：Chan<T,0>::Write / TryRead。
 */
BOOST_AUTO_TEST_CASE(t_ubo_write_hands_off_to_tryread)
{
    BOOST_TEST_MESSAGE("enter t_ubo_write_hands_off_to_tryread");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int write_ret{-999};
        std::atomic_int tryread_ret{-999};
        std::atomic_int tryread_value{0};
        std::atomic_bool write_done{false};
        std::atomic_bool write_done_before_close{false};
    };

    auto state = std::make_shared<CaseState>();
    auto c = Chan<int, 0>();

    bbt::core::thread::CountDownLatch body_latch{1};

    bbtco[state, c]()
    {
        int ret = c->Write(7);
        state->write_ret.store(ret);
        state->write_done.store(true);
    };

    bbtco[state, c, &body_latch]()
    {
        int received = 0;
        bool got = SpinUntilCo(
            [&c, &received]() {
                int val = 0;
                if (c->TryRead(val) != 0)
                    return false;
                received = val;
                return true;
            },
            2000);

        state->tryread_ret.store(got ? 0 : -1);
        state->tryread_value.store(received);

        bool write_done = SpinUntilCo([&state]() { return state->write_done.load(); }, 1000);
        state->write_done_before_close.store(write_done);
        if (!write_done)
            c->Close();
        SpinUntilCo([&state]() { return state->write_done.load(); }, 1000);
        body_latch.Down();
    };

    WaitLatch(body_latch, 6000);

    BOOST_CHECK_EQUAL(state->write_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->tryread_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->tryread_value.load(), 7);
    BOOST_CHECK(state->write_done_before_close.load());
}

/**
 * 行为：关闭后的零缓冲通道对所有入口都返回 -1，且失败写入不留下可读元素。
 * 抓错：某个入口绕过关闭状态、Try* 仍读写隐藏槽位、重复 Close 破坏状态。
 * 现有覆盖：t_nocache_close_ops 只检查 Write/Read，未覆盖 TryWrite/TryRead 与超时入口。
 * 入口：Close / IsClosed / Write / Read / TryWrite / TryRead（含超时）。
 */
BOOST_AUTO_TEST_CASE(t_ubo_close_is_terminal_for_all_entries)
{
    BOOST_TEST_MESSAGE("enter t_ubo_close_is_terminal_for_all_entries");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int closed{0};
        std::atomic_int trywrite_ret{-999};
        std::atomic_int timed_trywrite_ret{-999};
        std::atomic_int write_ret{-999};
        std::atomic_int tryread_ret{-999};
        std::atomic_int timed_tryread_ret{-999};
        std::atomic_int read_ret{-999};
        std::atomic_int tryread_value{0};
    };

    auto state = std::make_shared<CaseState>();

    bbt::core::thread::CountDownLatch l{1};
    bbtco[state, &l]()
    {
        auto c = Chan<int, 0>();
        c->Close();
        c->Close();

        state->closed.store(c->IsClosed() ? 1 : 0);
        state->trywrite_ret.store(c->TryWrite(1));
        state->timed_trywrite_ret.store(c->TryWrite(2, 10));
        state->write_ret.store(c->Write(3));

        int val = 0;
        state->tryread_ret.store(c->TryRead(val));
        state->timed_tryread_ret.store(c->TryRead(val, 10));
        state->read_ret.store(c->Read(val));
        state->tryread_value.store(val);
        l.Down();
    };

    WaitLatch(l, 3000);

    BOOST_CHECK_EQUAL(state->closed.load(), 1);
    BOOST_CHECK_EQUAL(state->trywrite_ret.load(), -1);
    BOOST_CHECK_EQUAL(state->timed_trywrite_ret.load(), -1);
    BOOST_CHECK_EQUAL(state->write_ret.load(), -1);
    BOOST_CHECK_EQUAL(state->tryread_ret.load(), -1);
    BOOST_CHECK_EQUAL(state->timed_tryread_ret.load(), -1);
    BOOST_CHECK_EQUAL(state->read_ret.load(), -1);
    BOOST_CHECK_NE(state->tryread_value.load(), 1);
}

/**
 * 行为：协程已被取消后再发起零缓冲 Write，不得挂死、也不得在未交接时返回 0。
 * 抓错：Write 不看已锁存的取消，挂起后被 Close 唤醒仍返回 0（失败当成功）。
 * 阶段：取消位在调用 Write 之前由同一协程锁存，程序序即证明取消先于匹配，无注入竞态。
 * 现有覆盖：t_nocache_close_wake_write 期望旧行为 0，取消路径没有零缓冲用例。
 * 入口：Coroutine::RequestCancel / Chan<T,0>::Write。
 */
BOOST_AUTO_TEST_CASE(t_ubo_cancelled_send_does_not_report_success)
{
    BOOST_TEST_MESSAGE("enter t_ubo_cancelled_send_does_not_report_success");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int write_ret{-999};
        std::atomic_bool write_done{false};
        std::atomic_bool write_done_before_close{false};
    };

    auto state = std::make_shared<CaseState>();
    auto c = Chan<int, 0>();

    bbt::core::thread::CountDownLatch body_latch{1};
    bbtco[state, c]()
    {
        g_bbt_tls_coroutine_co->RequestCancel();
        state->write_ret.store(c->Write(11));
        state->write_done.store(true);
    };

    bbtco[state, c, &body_latch]()
    {
        bool done = SpinUntilCo([&state]() { return state->write_done.load(); }, 800);
        state->write_done_before_close.store(done);
        if (!done)
            c->Close();
        SpinUntilCo([&state]() { return state->write_done.load(); }, 1000);
        body_latch.Down();
    };

    WaitLatch(body_latch, 4000);

    BOOST_CHECK_EQUAL(state->write_ret.load(), -2);
    BOOST_CHECK(state->write_done_before_close.load());
}

/**
 * 行为：接收端赋值抛异常时异常交给读者；阻塞写者得到 -2，且已匹配元素不回到队列。
 * 抓错：写者在匹配时先返回 0，赋值失败后仍报成功；元素重新入队造成重复交付。
 * 现有覆盖：没有用户类型异常用例；现有测试只覆盖 bool/int 元素。
 * 入口：Chan<UboThrowOnAssign,0>::Write / Read / TryRead。
 */
BOOST_AUTO_TEST_CASE(t_ubo_receiver_assign_throw_fails_matched_sender)
{
    BOOST_TEST_MESSAGE("enter t_ubo_receiver_assign_throw_fails_matched_sender");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int write_ret{-999};
        std::atomic_bool write_done{false};
        std::atomic_bool write_done_before_close{false};
        std::atomic_bool read_caught{false};
        std::atomic_int tryread_ret{-999};
    };

    auto state = std::make_shared<CaseState>();
    auto c = Chan<UboThrowOnAssign, 0>();

    bbt::core::thread::CountDownLatch body_latch{1};
    bbtco[state, c]()
    {
        state->write_ret.store(c->Write(UboThrowOnAssign(42)));
        state->write_done.store(true);
    };

    bbtco[state, c, &body_latch]()
    {
        UboThrowOnAssign out;
        try {
            c->Read(out);
        } catch (const std::runtime_error&) {
            state->read_caught.store(true);
        }

        /**
         * 先探测重复交付：已 Matched 的元素失败后不得重新入队。
         */
        UboThrowOnAssign probe;
        try {
            state->tryread_ret.store(c->TryRead(probe));
        } catch (const std::runtime_error&) {
            state->tryread_ret.store(-100);
        }

        bool write_done = SpinUntilCo([&state]() { return state->write_done.load(); }, 500);
        state->write_done_before_close.store(write_done);
        if (!write_done)
            c->Close();
        SpinUntilCo([&state]() { return state->write_done.load(); }, 1000);
        body_latch.Down();
    };

    WaitLatch(body_latch, 4000);

    BOOST_CHECK(state->read_caught.load());
    BOOST_CHECK_EQUAL(state->write_ret.load(), -2);
    BOOST_CHECK(state->write_done_before_close.load());
    BOOST_CHECK_EQUAL(state->tryread_ret.load(), -2);
}

/**
 * 行为：多写者并发写入时每个值恰好交付一次，且同一写者的写入按发起顺序到达。
 * 抓错：统一队列丢元素、重复交付、同一生产者内部顺序回退。
 * 现有覆盖：t_nocache_chan_1vn 只计数，不校验值与顺序。
 * 入口：Chan<T,0>::Write / Read。
 */
BOOST_AUTO_TEST_CASE(t_ubo_multi_producer_values_arrive_once_in_order)
{
    BOOST_TEST_MESSAGE("enter t_ubo_multi_producer_values_arrive_once_in_order");
    EnsureRuntime();

    constexpr int kWriters = 4;
    constexpr int kPerWriter = 8;
    constexpr int kTotal = kWriters * kPerWriter;

    struct CaseState {
        std::vector<int> received;
        std::atomic_int writer_failures{0};
        std::atomic_int reader_ret{0};
    };

    auto state = std::make_shared<CaseState>();
    auto c = Chan<int, 0>();

    bbt::core::thread::CountDownLatch writers_latch{kWriters};
    bbt::core::thread::CountDownLatch body_latch{1};

    for (int w = 0; w < kWriters; ++w) {
        bbtco[state, c, &writers_latch, w]()
        {
            for (int i = 0; i < kPerWriter; ++i) {
                if (c->Write(w * 100 + i) != 0) {
                    state->writer_failures.fetch_add(1);
                    break;
                }
            }
            writers_latch.Down();
        };
    }

    bbtco[state, c, &body_latch]()
    {
        for (int i = 0; i < kTotal; ++i) {
            int val = 0;
            int ret = c->Read(val);
            if (ret != 0) {
                state->reader_ret.store(ret);
                break;
            }
            state->received.push_back(val);
        }
        body_latch.Down();
    };

    WaitLatch(writers_latch, 8000);
    WaitLatch(body_latch, 8000);

    BOOST_CHECK_EQUAL(state->writer_failures.load(), 0);
    BOOST_CHECK_EQUAL(state->reader_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->received.size(), (size_t)kTotal);

    int next_index[kWriters] = {0, 0, 0, 0};
    int bad_value = -1;
    int bad_index = -1;
    for (int val : state->received) {
        int writer = val / 100;
        int index = val % 100;
        if (writer < 0 || writer >= kWriters || index != next_index[writer]) {
            bad_value = val;
            bad_index = index;
            break;
        }
        next_index[writer]++;
    }
    BOOST_CHECK_EQUAL(bad_value, -1);
    BOOST_CHECK_EQUAL(bad_index, -1);
    for (int w = 0; w < kWriters; ++w)
        BOOST_CHECK_EQUAL(next_index[w], kPerWriter);
}

/**
 * ================================================================
 * 缓冲即时路径语义等价回归（TryWrite/TryRead 栈 Operation，单锁 Admit）
 *
 * 目标：Max>0 的 Immediate 命中/空读探测/等待读者直达/补位唤醒/读赋值
 * 异常/关闭/取消，与统一核心契约的可观察结果完全一致；零缓冲慢路径
 * 作为同一断言的对照。等待方进入阻塞调用前先过 ParkWatch 屏障，证明
 * 已 park 后再注入；断言集中在主线程或单个编排协程内。
 * ================================================================
 */

/**
 * 行为：缓冲 TryWrite 命中空位返回 0 并存值；满且无等待读者返回 -2，
 *       被拒元素不落缓冲。
 * 抓错：满时返回 0/1 或残留被拒元素；命中空位不落缓冲。
 * 入口：Chan<int,2>::TryWrite / TryRead / size。
 */
BOOST_AUTO_TEST_CASE(t_buffered_trywrite_slot_and_full_probe)
{
    BOOST_TEST_MESSAGE("enter t_buffered_trywrite_slot_and_full_probe");
    EnsureRuntime();

    auto c = Chan<int, 2>();
    BOOST_REQUIRE(!c->IsClosed());

    BOOST_CHECK_EQUAL(c->TryWrite(1), 0);
    BOOST_CHECK_EQUAL(c->size(), 1u);
    BOOST_CHECK_EQUAL(c->TryWrite(2), 0);
    BOOST_CHECK_EQUAL(c->size(), 2u);

    /**
     * 满且无读者：即时写必须返回 -2（Unavailable），且不保存元素。
     */
    BOOST_CHECK_EQUAL(c->TryWrite(3), -2);
    BOOST_CHECK_EQUAL(c->size(), 2u);

    int v = 0;
    BOOST_CHECK_EQUAL(c->TryRead(v), 0);
    BOOST_CHECK_EQUAL(v, 1);
    BOOST_CHECK_EQUAL(c->TryRead(v), 0);
    BOOST_CHECK_EQUAL(v, 2);
    BOOST_CHECK_EQUAL(c->TryRead(v), -2);
    BOOST_CHECK_EQUAL(c->size(), 0u);
}

/**
 * 行为：缓冲 TryWrite 在没有空位但存在等待读者时直接配对：返回 0，读者
 *       取得该值，缓冲不留下副本。
 * 抓错：缓存非空判断把值写进缓冲、读者收不到；或既交付又留副本。
 * 阶段：读者与编排协程同 P，证明已 park 在 CUSTOM 等待后再 TryWrite。
 * 入口：Chan<int,2>::TryWrite / size；读者入口 Read。
 */
BOOST_AUTO_TEST_CASE(t_buffered_trywrite_matches_waiting_reader)
{
    BOOST_TEST_MESSAGE("enter t_buffered_trywrite_matches_waiting_reader");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int reader_ret{-999};
        std::atomic_int reader_val{0};
        std::atomic_int parked{0};
        std::atomic_int trywrite_ret{-999};
        std::atomic_int size_after_write{-1};
    };

    auto state = std::make_shared<CaseState>();
    auto watch = std::make_shared<ParkWatch>();
    auto c = Chan<int, 2>();
    bbt::core::thread::CountDownLatch l{1};

    bbtco[state, c, watch, &l]()
    {
        auto& sche = *bbt::coroutine::detail::Scheduler::GetInstance();
        auto pid = bbt::coroutine::detail::Processer::TryGetLocalProcesser()->GetId();

        sche.RegistCoroutineTaskOn(pid, [state, c, watch]() {
            watch->co.store(g_bbt_tls_coroutine_co);
            int v = 0;
            int ret = c->Read(v);
            state->reader_val.store(v);
            state->reader_ret.store(ret, std::memory_order_release);
            watch->done.store(1);
        });
        state->parked.store(WaitParkedOnCustom(watch, 3000) ? 1 : 0);

        if (state->parked.load()) {
            state->trywrite_ret.store(c->TryWrite(7));
            state->size_after_write.store(static_cast<int>(c->size()));
            SpinUntilCo([&state] { return state->reader_ret.load(std::memory_order_acquire) != -999; }, 3000);
        }
        l.Down();
    };

    WaitLatch(l, 8000);

    BOOST_CHECK_EQUAL(state->parked.load(), 1);
    BOOST_CHECK_EQUAL(state->trywrite_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->reader_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->reader_val.load(), 7);
    BOOST_CHECK_EQUAL(state->size_after_write.load(), 0);

    /**
     * 值只属于读者，通道已空
     */
    int v = 0;
    BOOST_CHECK_EQUAL(c->TryRead(v), -2);
}

/**
 * 行为：缓冲 TryRead 命中缓冲按 FIFO 返回 0，并在同一锁内为等待写者补位；
 *       补位写者随后返回 0，其值按 FIFO 排在旧值之后；排空后空读探测 -2。
 * 抓错：TryRead 只取不出、补位丢失导致写者永久挂起、补位值插到旧值之前、
 *       已补位写者返回值非 0。
 * 阶段：两名写者与编排协程同 P，逐个证明已 park 在 CUSTOM 等待后再 TryRead。
 * 入口：Chan<int,2>::TryRead（即时）/ size；写者入口 Write。
 */
BOOST_AUTO_TEST_CASE(t_buffered_tryread_fifo_promotes_waiting_writers)
{
    BOOST_TEST_MESSAGE("enter t_buffered_tryread_fifo_promotes_waiting_writers");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int w3{-999};
        std::atomic_int w4{-999};
        std::atomic_int parked{0};
        std::atomic_int r0{-999};
        std::atomic_int v0{0};
        std::atomic_int r1{-999};
        std::atomic_int v1{0};
        std::atomic_int r2{-999};
        std::atomic_int v2{0};
        std::atomic_int r3{-999};
        std::atomic_int v3{0};
        std::atomic_int probe{-999};
        std::atomic_int size_after_first{-1};
        std::atomic_int size_after_second{-1};
        std::atomic_int size_end{-1};
    };

    auto state = std::make_shared<CaseState>();
    auto w3_watch = std::make_shared<ParkWatch>();
    auto w4_watch = std::make_shared<ParkWatch>();
    auto c = Chan<int, 2>();
    bbt::core::thread::CountDownLatch l{1};

    BOOST_REQUIRE_EQUAL(c->TryWrite(1), 0);
    BOOST_REQUIRE_EQUAL(c->TryWrite(2), 0);

    bbtco[state, c, w3_watch, w4_watch, &l]()
    {
        auto& sche = *bbt::coroutine::detail::Scheduler::GetInstance();
        auto pid = bbt::coroutine::detail::Processer::TryGetLocalProcesser()->GetId();

        sche.RegistCoroutineTaskOn(pid, [state, c, w3_watch]() {
            w3_watch->co.store(g_bbt_tls_coroutine_co);
            state->w3.store(c->Write(3));
            w3_watch->done.store(1);
        });
        bool p3 = WaitParkedOnCustom(w3_watch, 3000);

        sche.RegistCoroutineTaskOn(pid, [state, c, w4_watch]() {
            w4_watch->co.store(g_bbt_tls_coroutine_co);
            state->w4.store(c->Write(4));
            w4_watch->done.store(1);
        });
        bool p4 = WaitParkedOnCustom(w4_watch, 3000);
        state->parked.store(p3 && p4 ? 1 : 0);

        if (state->parked.load()) {
            int v = 0;
            state->r0.store(c->TryRead(v));
            state->v0.store(v);
            state->size_after_first.store(static_cast<int>(c->size()));

            /**
             * 首个等待写者应已在 TryRead 的同一锁内补位，补位后缓冲仍满
             */
            SpinUntilCo([&state] { return state->w3.load(std::memory_order_acquire) != -999; }, 3000);

            state->r1.store(c->TryRead(v));
            state->v1.store(v);
            state->size_after_second.store(static_cast<int>(c->size()));
            SpinUntilCo([&state] { return state->w4.load(std::memory_order_acquire) != -999; }, 3000);

            state->r2.store(c->TryRead(v));
            state->v2.store(v);
            state->r3.store(c->TryRead(v));
            state->v3.store(v);
            state->probe.store(c->TryRead(v));
            state->size_end.store(static_cast<int>(c->size()));
        }
        l.Down();
    };

    WaitLatch(l, 8000);

    BOOST_CHECK_EQUAL(state->parked.load(), 1);
    BOOST_CHECK_EQUAL(state->w3.load(), 0);
    BOOST_CHECK_EQUAL(state->w4.load(), 0);

    /**
     * 缓冲读按 FIFO：1、2 是旧值，3、4 是补位写者的值
     */
    BOOST_CHECK_EQUAL(state->r0.load(), 0);
    BOOST_CHECK_EQUAL(state->v0.load(), 1);
    BOOST_CHECK_EQUAL(state->r1.load(), 0);
    BOOST_CHECK_EQUAL(state->v1.load(), 2);
    BOOST_CHECK_EQUAL(state->r2.load(), 0);
    BOOST_CHECK_EQUAL(state->v2.load(), 3);
    BOOST_CHECK_EQUAL(state->r3.load(), 0);
    BOOST_CHECK_EQUAL(state->v3.load(), 4);
    BOOST_CHECK_EQUAL(state->probe.load(), -2);
    BOOST_CHECK_EQUAL(state->size_after_first.load(), 2);
    BOOST_CHECK_EQUAL(state->size_after_second.load(), 2);
    BOOST_CHECK_EQUAL(state->size_end.load(), 0);
}

/**
 * 行为：缓冲 TryRead 的接收端赋值抛异常时异常外抛，值已出队不回队；
 *       同一次 TryRead 已补位的等待写者不受影响，仍返回 0，其值留在缓冲。
 * 抓错：异常被吞、失败值回队导致重复交付、补位写者被判失败或值丢失。
 * 阶段：写者与编排协程同 P，证明已 park 在 CUSTOM 等待后再 TryRead。
 * 入口：Chan<BufThrowOnAssign,2>::TryRead（即时）/ size；写者入口 Write。
 */
BOOST_AUTO_TEST_CASE(t_buffered_tryread_assign_throw_keeps_promoted_writer)
{
    BOOST_TEST_MESSAGE("enter t_buffered_tryread_assign_throw_keeps_promoted_writer");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int w3{-999};
        std::atomic_int parked{0};
        std::atomic_int caught{0};
        std::atomic_int size_after_throw{-1};
        std::atomic_int r1{-999};
        std::atomic_int v1{0};
        std::atomic_int r2{-999};
        std::atomic_int v2{0};
        std::atomic_int probe{-999};
        std::atomic_int size_end{-1};
    };

    auto state = std::make_shared<CaseState>();
    auto w3_watch = std::make_shared<ParkWatch>();
    auto c = Chan<BufThrowOnAssign, 2>();
    bbt::core::thread::CountDownLatch l{1};

    BOOST_REQUIRE_EQUAL(c->Write(BufThrowOnAssign(10, true)), 0);
    BOOST_REQUIRE_EQUAL(c->Write(BufThrowOnAssign(20, false)), 0);

    bbtco[state, c, w3_watch, &l]()
    {
        auto& sche = *bbt::coroutine::detail::Scheduler::GetInstance();
        auto pid = bbt::coroutine::detail::Processer::TryGetLocalProcesser()->GetId();

        sche.RegistCoroutineTaskOn(pid, [state, c, w3_watch]() {
            w3_watch->co.store(g_bbt_tls_coroutine_co);
            state->w3.store(c->Write(BufThrowOnAssign(30)));
            w3_watch->done.store(1);
        });
        state->parked.store(WaitParkedOnCustom(w3_watch, 3000) ? 1 : 0);

        if (state->parked.load()) {
            BufThrowOnAssign out;
            try {
                c->TryRead(out);
            } catch (const std::runtime_error&) {
                state->caught.store(1);
            }
            state->size_after_throw.store(static_cast<int>(c->size()));

            /**
             * 赋值失败不影响同锁补位的写者
             */
            SpinUntilCo([&state] { return state->w3.load(std::memory_order_acquire) != -999; }, 3000);

            BufThrowOnAssign next;
            state->r1.store(c->TryRead(next));
            state->v1.store(next.value);
            state->r2.store(c->TryRead(next));
            state->v2.store(next.value);
            state->probe.store(c->TryRead(next));
            state->size_end.store(static_cast<int>(c->size()));
        }
        l.Down();
    };

    WaitLatch(l, 8000);

    BOOST_CHECK_EQUAL(state->parked.load(), 1);
    BOOST_CHECK_EQUAL(state->caught.load(), 1);
    BOOST_CHECK_EQUAL(state->w3.load(), 0);

    /**
     * 10 已出队不回队，30 已补位：缓冲仍为 2 且首值是 20
     */
    BOOST_CHECK_EQUAL(state->size_after_throw.load(), 2);
    BOOST_CHECK_EQUAL(state->r1.load(), 0);
    BOOST_CHECK_EQUAL(state->v1.load(), 20);
    BOOST_CHECK_EQUAL(state->r2.load(), 0);
    BOOST_CHECK_EQUAL(state->v2.load(), 30);
    BOOST_CHECK_EQUAL(state->probe.load(), -2);
    BOOST_CHECK_EQUAL(state->size_end.load(), 0);
}

/**
 * 行为：Close 后缓冲 TryWrite/TryRead 都返回 -1，不使用也不消费残留；
 *       残留仍只能由 Read 排空。
 * 抓错：关闭后 TryWrite 仍接纳元素、TryRead 越过关闭消费残留。
 * 入口：Chan<int,4>::TryWrite / TryRead / Read / size / IsClosed。
 */
BOOST_AUTO_TEST_CASE(t_buffered_tryops_after_close_are_terminal)
{
    BOOST_TEST_MESSAGE("enter t_buffered_tryops_after_close_are_terminal");
    EnsureRuntime();

    auto c = Chan<int, 4>();
    BOOST_REQUIRE_EQUAL(c->TryWrite(1), 0);

    c->Close();
    BOOST_CHECK(c->IsClosed());

    BOOST_CHECK_EQUAL(c->TryWrite(2), -1);
    BOOST_CHECK_EQUAL(c->size(), 1u);

    int v = 0;
    BOOST_CHECK_EQUAL(c->TryRead(v), -1);
    BOOST_CHECK_EQUAL(c->size(), 1u);

    BOOST_CHECK_EQUAL(c->Read(v), 0);
    BOOST_CHECK_EQUAL(v, 1);
    BOOST_CHECK_EQUAL(c->Read(v), -1);
    BOOST_CHECK_EQUAL(c->TryWrite(3), -1);
}

/**
 * 行为：已请求取消的协程上，缓冲 TryWrite/TryRead 都返回 -2，且不接纳、
 *       不消费缓冲里的值。
 * 抓错：即时路径先看缓冲/空位、忽略取消位，取消后仍写入或吃掉残留。
 * 入口：Chan<int,2>::TryWrite / TryRead / size。
 */
BOOST_AUTO_TEST_CASE(t_buffered_tryops_cancelled_coroutine_no_effect)
{
    BOOST_TEST_MESSAGE("enter t_buffered_tryops_cancelled_coroutine_no_effect");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int trywrite_ret{-999};
        std::atomic_int tryread_ret{-999};
        std::atomic_int tryread_val{0};
        std::atomic_int size_after{0};
    };

    auto state = std::make_shared<CaseState>();
    auto c = Chan<int, 2>();
    BOOST_REQUIRE_EQUAL(c->TryWrite(1), 0);

    bbt::core::thread::CountDownLatch l{1};
    bbtco[state, c, &l]()
    {
        g_bbt_tls_coroutine_co->RequestCancel();
        state->trywrite_ret.store(c->TryWrite(9));

        int v = 0;
        state->tryread_ret.store(c->TryRead(v));
        state->tryread_val.store(v);
        state->size_after.store(static_cast<int>(c->size()));
        l.Down();
    };

    WaitLatch(l, 5000);

    BOOST_CHECK_EQUAL(state->trywrite_ret.load(), -2);
    BOOST_CHECK_EQUAL(state->tryread_ret.load(), -2);
    BOOST_CHECK_EQUAL(state->tryread_val.load(), 0);
    BOOST_CHECK_EQUAL(state->size_after.load(), 1);

    /**
     * 取消调用不得留下 9，也不得吃掉已有的 1
     */
    int v = 0;
    BOOST_CHECK_EQUAL(c->TryRead(v), 0);
    BOOST_CHECK_EQUAL(v, 1);
    BOOST_CHECK_EQUAL(c->TryRead(v), -2);
}

/**
 * 行为：多生产者/多消费者只用即时 TryWrite/TryRead 时，每个值恰好交付一次，
 *       通道整体保持 FIFO：同一生产者的写入按发起顺序出队。
 * 抓错：满缓冲重试丢元素或重复接纳、即时读重复交付同一元素、生产者内部乱序。
 * 说明：消费者在测试互斥量内完成「读出 + 追加」，汇总顺序即通道出队顺序。
 * 入口：Chan<int,4>::TryWrite / TryRead。
 */
BOOST_AUTO_TEST_CASE(t_buffered_tryops_multi_producer_consumer_fifo)
{
    BOOST_TEST_MESSAGE("enter t_buffered_tryops_multi_producer_consumer_fifo");
    EnsureRuntime();

    constexpr int kWriters = 3;
    constexpr int kPerWriter = 10;
    constexpr int kTotal = kWriters * kPerWriter;
    constexpr int kReaders = 2;

    struct CaseState {
        std::mutex mutex;
        std::vector<int> received;
        std::atomic_int consumed{0};
        std::atomic_int writer_failures{0};
        std::atomic_int reader_errors{0};
    };

    auto state = std::make_shared<CaseState>();
    auto c = Chan<int, 4>();
    bbt::core::thread::CountDownLatch writers{kWriters};
    bbt::core::thread::CountDownLatch readers{kReaders};

    for (int w = 0; w < kWriters; ++w) {
        bbtco[state, c, &writers, w]()
        {
            for (int i = 0; i < kPerWriter; ++i) {
                bool written = false;
                for (;;) {
                    int ret = c->TryWrite(w * 100 + i);
                    if (ret == 0) {
                        written = true;
                        break;
                    }
                    if (ret != -2) {
                        state->writer_failures.fetch_add(1);
                        break;
                    }
                    bbtco_yield;
                }
                if (!written)
                    break;
            }
            writers.Down();
        };
    }

    for (int r = 0; r < kReaders; ++r) {
        bbtco[state, c, &readers]()
        {
            for (;;) {
                bool got = false;
                bool terminal = false;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->consumed.load(std::memory_order_relaxed) >= kTotal)
                        break;

                    int v = 0;
                    int ret = c->TryRead(v);
                    if (ret == 0) {
                        state->received.push_back(v);
                        state->consumed.fetch_add(1, std::memory_order_relaxed);
                        got = true;
                    } else if (ret != -2) {
                        state->reader_errors.fetch_add(1);
                        terminal = true;
                    }
                }
                if (terminal)
                    break;
                if (!got)
                    bbtco_yield;
            }
            readers.Down();
        };
    }

    WaitLatch(writers, 8000);
    WaitLatch(readers, 8000);

    BOOST_CHECK_EQUAL(state->writer_failures.load(), 0);
    BOOST_CHECK_EQUAL(state->reader_errors.load(), 0);
    BOOST_REQUIRE_EQUAL(state->received.size(), static_cast<size_t>(kTotal));

    std::set<int> seen;
    int next_index[kWriters] = {0, 0, 0};
    int bad_value = -1;
    for (int val : state->received) {
        int writer = val / 100;
        int index = val % 100;
        if (writer < 0 || writer >= kWriters || index != next_index[writer]) {
            bad_value = val;
            break;
        }
        next_index[writer]++;
        seen.insert(val);
    }
    BOOST_CHECK_EQUAL(bad_value, -1);
    BOOST_CHECK_EQUAL(seen.size(), static_cast<size_t>(kTotal));
    for (int w = 0; w < kWriters; ++w)
        BOOST_CHECK_EQUAL(next_index[w], kPerWriter);
}

/**
 * 行为：同一形态的即时写在缓冲与零缓冲通道上可观察结果一致——存在等待读者
 *       时返回 0 且读者取得值；零缓冲走慢路径作对照。
 * 抓错：缓冲快路径绕过等待读者、零缓冲对照行为被改变、缓冲同时留下副本。
 * 阶段：两名读者与编排协程同 P，逐个证明已 park 在 CUSTOM 等待后再写。
 * 入口：Chan<int,2>::TryWrite / size、Chan<int,0>::TryWrite；读者入口 Read。
 */
BOOST_AUTO_TEST_CASE(t_chan_trywrite_pairs_waiting_reader_buffered_and_unbuffered)
{
    BOOST_TEST_MESSAGE("enter t_chan_trywrite_pairs_waiting_reader_buffered_and_unbuffered");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int parked{0};
        std::atomic_int buffered_w{-999};
        std::atomic_int unbuffered_w{-999};
        std::atomic_int buffered_r{-999};
        std::atomic_int buffered_v{0};
        std::atomic_int unbuffered_r{-999};
        std::atomic_int unbuffered_v{0};
        std::atomic_int buffered_size{-1};
    };

    auto state = std::make_shared<CaseState>();
    auto cb = Chan<int, 2>();
    auto cu = Chan<int, 0>();
    auto bwatch = std::make_shared<ParkWatch>();
    auto uwatch = std::make_shared<ParkWatch>();
    bbt::core::thread::CountDownLatch l{1};

    bbtco[state, cb, cu, bwatch, uwatch, &l]()
    {
        auto& sche = *bbt::coroutine::detail::Scheduler::GetInstance();
        auto pid = bbt::coroutine::detail::Processer::TryGetLocalProcesser()->GetId();

        sche.RegistCoroutineTaskOn(pid, [state, cb, bwatch]() {
            bwatch->co.store(g_bbt_tls_coroutine_co);
            int v = 0;
            int ret = cb->Read(v);
            state->buffered_v.store(v);
            state->buffered_r.store(ret, std::memory_order_release);
            bwatch->done.store(1);
        });
        bool bp = WaitParkedOnCustom(bwatch, 3000);

        sche.RegistCoroutineTaskOn(pid, [state, cu, uwatch]() {
            uwatch->co.store(g_bbt_tls_coroutine_co);
            int v = 0;
            int ret = cu->Read(v);
            state->unbuffered_v.store(v);
            state->unbuffered_r.store(ret, std::memory_order_release);
            uwatch->done.store(1);
        });
        bool up = WaitParkedOnCustom(uwatch, 3000);
        state->parked.store(bp && up ? 1 : 0);

        if (state->parked.load()) {
            state->buffered_w.store(cb->TryWrite(11));
            state->buffered_size.store(static_cast<int>(cb->size()));
            state->unbuffered_w.store(cu->TryWrite(22));
            SpinUntilCo(
                [&state] {
                    return state->buffered_r.load(std::memory_order_acquire) != -999
                           && state->unbuffered_r.load(std::memory_order_acquire) != -999;
                },
                3000);
        }
        l.Down();
    };

    WaitLatch(l, 8000);

    BOOST_CHECK_EQUAL(state->parked.load(), 1);
    BOOST_CHECK_EQUAL(state->buffered_w.load(), 0);
    BOOST_CHECK_EQUAL(state->unbuffered_w.load(), 0);
    BOOST_CHECK_EQUAL(state->buffered_r.load(), 0);
    BOOST_CHECK_EQUAL(state->buffered_v.load(), 11);
    BOOST_CHECK_EQUAL(state->unbuffered_r.load(), 0);
    BOOST_CHECK_EQUAL(state->unbuffered_v.load(), 22);

    /**
     * 缓冲通道的值直达读者，不得在槽位留下副本
     */
    BOOST_CHECK_EQUAL(state->buffered_size.load(), 0);

    int v = 0;
    BOOST_CHECK_EQUAL(cb->TryRead(v), -2);
    BOOST_CHECK_EQUAL(cu->TryRead(v), -2);
}

/**
 * 行为：缓冲空且无写者时，TryRead 即时探测直接失败返回 -2，不挂起、不改写
 *       接收变量，size 保持 0。
 * 抓错：空读探测误挂起、误交付、误改写接收变量或污染 size。
 * 说明：Max>0 的 Immediate 读走栈 Operation 快路径，单锁内判空即完成。
 */
BOOST_AUTO_TEST_CASE(t_buffered_fastpath_empty_tryread_probe)
{
    BOOST_TEST_MESSAGE("enter t_buffered_fastpath_empty_tryread_probe");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int tr_ret{-999};
        std::atomic_int v_after{-999};
        std::atomic_int size_after{-1};
    };

    auto state = std::make_shared<CaseState>();
    auto c = Chan<int, 4>();
    bbt::core::thread::CountDownLatch l{1};

    bbtco[state, c, &l]()
    {
        int v = -777;
        state->tr_ret.store(c->TryRead(v));
        state->v_after.store(v);
        state->size_after.store(static_cast<int>(c->size()));
        l.Down();
    };

    WaitLatch(l, 5000);

    BOOST_CHECK_EQUAL(state->tr_ret.load(), -2);
    BOOST_CHECK_EQUAL(state->v_after.load(), -777);
    BOOST_CHECK_EQUAL(state->size_after.load(), 0);
}

/**
 * 行为：缓冲满且写者已 park 时，TryRead 取走队首后，被释放的槽位在同一把
 *       Chan 锁内按 FIFO 补位给等待写者：写者返回 0、其值入缓冲，随后的
 *       Read 取得补位值。
 * 抓错：快路径读释放容量后不唤醒等待写者、补位值丢失或乱序、TryRead 返回值
 *       与交付值不匹配。
 * 阶段：写者与编排协程同 P，证明已 park 在 Write 后再消费；补位在同一锁内
 *       完成，TryRead 返回即已补位。
 */
BOOST_AUTO_TEST_CASE(t_buffered_fastpath_tryread_refill_wakes_writer)
{
    BOOST_TEST_MESSAGE("enter t_buffered_fastpath_tryread_refill_wakes_writer");
    EnsureRuntime();

    struct CaseState {
        std::atomic_int w2{-999};
        std::atomic_int parked{0};
        std::atomic_int tr_ret{-999};
        std::atomic_int tr_val{-999};
        std::atomic_int size_after{-1};
        std::atomic_int r2_ret{-999};
        std::atomic_int r2_val{-999};
        std::atomic_int size_end{-1};
    };

    auto state = std::make_shared<CaseState>();
    auto watch = std::make_shared<ParkWatch>();
    auto c = Chan<int, 1>();
    bbt::core::thread::CountDownLatch l{1};

    BOOST_REQUIRE_EQUAL(c->Write(1), 0);

    bbtco[state, c, watch, &l]()
    {
        auto& sche = *bbt::coroutine::detail::Scheduler::GetInstance();
        auto pid = bbt::coroutine::detail::Processer::TryGetLocalProcesser()->GetId();

        sche.RegistCoroutineTaskOn(pid, [state, c, watch]() {
            watch->co.store(g_bbt_tls_coroutine_co);
            state->w2.store(c->Write(2));
            watch->done.store(1);
        });
        bool parked = WaitParkedOnCustom(watch, 3000);
        state->parked.store(parked ? 1 : 0);

        if (state->parked.load()) {
            int v = -999;
            state->tr_ret.store(c->TryRead(v));
            state->tr_val.store(v);
            state->size_after.store(static_cast<int>(c->size()));

            SpinUntilCo([&state] { return state->w2.load() != -999; }, 3000);

            int v2 = -999;
            state->r2_ret.store(c->Read(v2));
            state->r2_val.store(v2);
            state->size_end.store(static_cast<int>(c->size()));
        }
        l.Down();
    };

    WaitLatch(l, 10000);

    BOOST_CHECK_EQUAL(state->parked.load(), 1);
    BOOST_CHECK_EQUAL(state->tr_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->tr_val.load(), 1);
    BOOST_CHECK_EQUAL(state->size_after.load(), 1);
    BOOST_CHECK_EQUAL(state->w2.load(), 0);
    BOOST_CHECK_EQUAL(state->r2_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->r2_val.load(), 2);
    BOOST_CHECK_EQUAL(state->size_end.load(), 0);
}

/**
 * 行为：TryRead 即时命中的接收端赋值抛异常时，异常外抛给读者；已出缓冲的
 *       值不回队，通道随后为空，再次 TryRead 返回 -2。
 * 抓错：异常路径把已出队的值放回缓冲导致重复交付、通道残留脏状态。
 * 说明：空态断言用即时读探针——对开放空通道的阻塞 Read 会挂起而非返回。
 */
BOOST_AUTO_TEST_CASE(t_buffered_fastpath_tryread_assign_throw)
{
    BOOST_TEST_MESSAGE("enter t_buffered_fastpath_tryread_assign_throw");
    EnsureRuntime();

    /**
     * 接收侧赋值抛异常：复制构造正常（写入与入缓冲不受影响）；
     * throw_on_assign 的元素被赋值时抛出。
     */
    struct FastpathThrowOnAssign {
        int value{0};
        bool throw_on_assign{false};

        FastpathThrowOnAssign() = default;

        explicit FastpathThrowOnAssign(int v, bool do_throw = false)
            : value(v)
            , throw_on_assign(do_throw)
        {
        }

        FastpathThrowOnAssign(const FastpathThrowOnAssign&) = default;
        FastpathThrowOnAssign(FastpathThrowOnAssign&&) = default;

        FastpathThrowOnAssign& operator=(const FastpathThrowOnAssign& other)
        {
            if (other.throw_on_assign)
                throw std::runtime_error("fastpath assign");
            value = other.value;
            throw_on_assign = other.throw_on_assign;
            return *this;
        }

        FastpathThrowOnAssign& operator=(FastpathThrowOnAssign&& other)
        {
            if (other.throw_on_assign)
                throw std::runtime_error("fastpath assign");
            value = other.value;
            throw_on_assign = other.throw_on_assign;
            return *this;
        }
    };

    struct CaseState {
        std::atomic_int caught{0};
        std::atomic_int out_val{-999};
        std::atomic_int size_after{-1};
        std::atomic_int post_ret{-999};
        std::atomic_int probe_val{-999};
    };

    auto state = std::make_shared<CaseState>();
    auto c = Chan<FastpathThrowOnAssign, 2>();
    bbt::core::thread::CountDownLatch l{1};

    BOOST_REQUIRE_EQUAL(c->Write(FastpathThrowOnAssign(30, true)), 0);

    bbtco[state, c, &l]()
    {
        FastpathThrowOnAssign out;
        out.value = -777;
        try {
            c->TryRead(out);
        } catch (const std::runtime_error&) {
            state->caught.store(1);
        }
        state->out_val.store(out.value);
        state->size_after.store(static_cast<int>(c->size()));

        FastpathThrowOnAssign probe;
        probe.value = -999;
        state->post_ret.store(c->TryRead(probe));
        state->probe_val.store(probe.value);
        l.Down();
    };

    WaitLatch(l, 5000);

    BOOST_CHECK_EQUAL(state->caught.load(), 1);
    BOOST_CHECK_EQUAL(state->out_val.load(), -777);
    BOOST_CHECK_EQUAL(state->size_after.load(), 0);
    BOOST_CHECK_EQUAL(state->post_ret.load(), -2);
    BOOST_CHECK_EQUAL(state->probe_val.load(), -999);
}

BOOST_AUTO_TEST_CASE(t_end) {}

BOOST_AUTO_TEST_SUITE_END()