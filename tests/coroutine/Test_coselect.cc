#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <stdexcept>
#include <vector>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/sync/CoSelect.hpp>

/**
 * 进程寿命模型：runtime 只初始化一次，重复 Start 抛 std::logic_error。
 * 每个测试文件就是一个可执行，这里把用例内的 Start() 收敛为进程内一次初始化。
 * 固定单 worker：零缓冲竞态用例靠同 P 串行顺序做见证，多 worker 时并发恢复
 * 不受控；配置必须在首次 Start 固化前写入，所以放进 call_once。
 */
namespace {
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, []() {
        g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
        bbt::coroutine::detail::Scheduler::GetInstance()->Start();
    });
}
} // namespace

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(CoSelectTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    EnsureRuntime();
}

/**
 * 两路 CaseRead：只往 ch1 写，Run 返回 0，out 正确；且必须被写入唤醒而非超时兜底
 */
BOOST_AUTO_TEST_CASE(t_select_case_read)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    int out1 = 0;
    int out2 = 0;
    int elapsed_ms = -1;

    bbtco[&]()
    {
        sync::Chan<int, 4> ch1;
        sync::Chan<int, 4> ch2;

        bbtco[&ch1]()
        {
            /**
             * 写入故意滞后于 select 发起，等待中的分支也必须完成交付；
             * 行为断言只看交付完成与及时性，不依赖具体唤醒机制。
             */
            bbtco_sleep(30);
            BOOST_CHECK_EQUAL(ch1.TryWrite(42), 0);
        };

        auto begin = bbt::core::clock::gettime_mono();
        index = sync::CoSelect().CaseRead(ch1, out1).CaseRead(ch2, out2).CaseTimeout(2000).Run();
        elapsed_ms = bbt::core::clock::gettime_mono() - begin;
        l.Down();
    };

    l.Wait();
    BOOST_CHECK_EQUAL(index, 0);
    BOOST_CHECK_EQUAL(out1, 42);

    /**
     * 写入发生在 30ms：唤醒必须及时，不能靠 2000ms 超时后的重试兜底（唤醒及时性）
     */
    BOOST_CHECK_LT(elapsed_ms, 500);
}

/**
 * 两路都已就绪时使用伪随机顺序选择，返回原始注册下标；重复选择不能长期偏向同一个分支。
 */
BOOST_AUTO_TEST_CASE(t_select_both_ready_pseudorandom)
{
    bbt::core::thread::CountDownLatch l{1};
    int first_count = 0;
    int second_count = 0;

    bbtco[&]()
    {
        for (int i = 0; i < 64; ++i) {
            sync::Chan<int, 1> ch1;
            sync::Chan<int, 1> ch2;
            BOOST_CHECK_EQUAL(ch1.TryWrite(11), 0);
            BOOST_CHECK_EQUAL(ch2.TryWrite(22), 0);

            int out1 = 0;
            int out2 = 0;
            const int index = sync::CoSelect().CaseRead(ch1, out1).CaseRead(ch2, out2).Run();
            if (index == 0) {
                BOOST_CHECK_EQUAL(out1, 11);
                ++first_count;
            } else if (index == 1) {
                BOOST_CHECK_EQUAL(out2, 22);
                ++second_count;
            } else {
                BOOST_ERROR("both-ready select returned no case");
                break;
            }
        }
        l.Down();
    };

    l.Wait();
    BOOST_CHECK_GT(first_count, 0);
    BOOST_CHECK_GT(second_count, 0);
}

/**
 * Default + 空 chan：立即 -1，不挂起
 */
BOOST_AUTO_TEST_CASE(t_select_default)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    int elapsed = -1;

    bbtco[&]()
    {
        sync::Chan<int, 4> ch;
        int out = 0;
        auto begin = bbt::core::clock::gettime_mono();
        index = sync::CoSelect().CaseRead(ch, out).Default().Run();
        elapsed = bbt::core::clock::gettime_mono() - begin;
        l.Down();
    };

    l.Wait();
    BOOST_CHECK_EQUAL(index, -1);
    BOOST_CHECK_LT(elapsed, 50);
}

/**
 * CaseTimeout(50) + 空 chan：返回 -1，耗时 >=40ms 且 <1000ms
 */
BOOST_AUTO_TEST_CASE(t_select_timeout)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    int elapsed = -1;

    bbtco[&]()
    {
        sync::Chan<int, 4> ch;
        int out = 0;
        auto begin = bbt::core::clock::gettime_mono();
        index = sync::CoSelect().CaseRead(ch, out).CaseTimeout(50).Run();
        elapsed = bbt::core::clock::gettime_mono() - begin;
        l.Down();
    };

    l.Wait();
    BOOST_CHECK_EQUAL(index, -1);
    BOOST_CHECK_GE(elapsed, 40);
    BOOST_CHECK_LT(elapsed, 1000);
}

/**
 * CaseWrite：满缓冲的 chan 等读端腾出后写成功
 */
BOOST_AUTO_TEST_CASE(t_select_case_write)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    int written = 0;

    bbtco[&]()
    {
        sync::Chan<int, 1> ch;

        /**
         * 填满
         */
        BOOST_CHECK_EQUAL(ch.TryWrite(99), 0);

        bbtco[&ch]()
        {
            /**
             * 等 select 挂起后腾出缓冲
             */
            bbtco_sleep(30);
            int v = 0;
            BOOST_CHECK_EQUAL(ch.Read(v), 0);
            BOOST_CHECK_EQUAL(v, 99);
        };

        index = sync::CoSelect().CaseWrite(ch, 7).CaseTimeout(2000).Run();

        /**
         * 写命中后 7 应在队列里
         */
        int out = 0;
        written = ch.TryRead(out);
        BOOST_CHECK_EQUAL(out, 7);
        l.Down();
    };

    l.Wait();
    BOOST_CHECK_EQUAL(index, 0);
    BOOST_CHECK_EQUAL(written, 0);
}

/**
 * 挂起期间 ticker 协程仍在跑（调度没有卡死）
 */
BOOST_AUTO_TEST_CASE(t_select_ticker_alive_while_waiting)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    std::atomic_int tick_count{0};

    bbtco[&]()
    {
        std::atomic_bool ticker_done{false};
        bbtco[&tick_count, &ticker_done]()
        {
            for (int i = 0; i < 30; ++i) {
                bbtco_sleep(10);
                ++tick_count;
            }
            ticker_done = true;
        };

        sync::Chan<int, 4> ch;
        int out = 0;
        index = sync::CoSelect().CaseRead(ch, out).CaseTimeout(200).Run();

        /**
         * 等 ticker 收尾，避免栈上引用悬垂
         */
        while (!ticker_done)
            bbtco_sleep(10);
        l.Down();
    };

    l.Wait();

    /**
     * 无数据 → 超时
     */
    BOOST_CHECK_EQUAL(index, -1);
    BOOST_CHECK_GT(tick_count.load(), 0);
}

/**
 * Close 唤醒：select 等空 chan（无超时），另一协程 Close，Run 快速返回
 */
BOOST_AUTO_TEST_CASE(t_select_close_wakeup)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    int elapsed = -1;

    bbtco[&]()
    {
        sync::Chan<int, 4> ch;
        int out = 0;

        bbtco[&ch]()
        {
            bbtco_sleep(30);
            ch.Close();
        };

        auto begin = bbt::core::clock::gettime_mono();

        /**
         * 故意不给 CaseTimeout：只能被 Close 唤醒
         */
        index = sync::CoSelect().CaseRead(ch, out).Run();
        elapsed = bbt::core::clock::gettime_mono() - begin;
        l.Down();
    };

    l.Wait();

    /**
     * closed 视为就绪
     */
    BOOST_CHECK_EQUAL(index, 0);
    BOOST_CHECK_LT(elapsed, 1000);
}

/**
 * ================================================================
 * 零缓冲 CoSelect 回归
 *
 * 目标契约：零缓冲通道上 select 分支与普通读写、互补 select 共用同一交接
 * 与首胜仲裁——一次配对只交付一次、返回命中分支下标；交付发生在调用 Run
 * 的协程上；超时/取消先胜后迟到的对端不得完成交接。
 *
 * 竞态见证方法（单 worker）：R 进 Run 前把见证/对端协程排入自己所在 P 的
 * 就绪队列尾部。就绪队列 FIFO，见证被调度时 R 必然已挂起或已返回（哨兵
 * 区分），同一时刻 R 不在执行，见证对 R 指针的操作与共享状态访问都在同 P
 * 串行线上，不做跨线程检查；主线程读回结果由 latch 的 Mutex 配对同步，
 * 共享状态不需要 atomic。
 * ================================================================
 */
namespace {

template <class F> void EnqueuePeer(const F& fn)
{
    detail::Scheduler::GetInstance()->RegistCoroutineTaskOn(
        detail::Processer::GetLocalProcesser()->GetId(), detail::CoroutineCallback(fn), "coselect-peer");
}

/**
 * 接收侧赋值抛异常的元素类型：复制构造正常，赋值固定抛出。
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

struct RaceResult {
    int run_ret{-999};
    int zb_probe{-999};
    int drain{-999};
    int final_read{-999};
};

/**
 * 取消/超时先胜竞态共用编排：R 在空 ch_zb 上 CaseRead、在满 ch_buf 上
 * CaseWrite(99)，两者都不就绪而挂起；见证在 R 挂起期间独占 worker，
 * cancel_first 时先请求取消，再以不让出的 cv 等待覆盖超时窗口（逾期首胜
 * 由截止时间仲裁，与定时器投递时点无关）；随后不 yield 地探测：ch_zb 迟到
 * 写必须配对失败（-2），排空 ch_buf 预填使恢复后重跑就绪检查有机会写入 99。
 * 见证退出后 R 才能恢复，全部探测先于恢复是调度顺序保证，无需额外断言。
 */
RaceResult RunFirstWinRace(bool cancel_first, int timeout_ms)
{
    RaceResult r;
    auto ch_zb = std::make_shared<sync::Chan<int, 0>>();
    auto ch_buf = std::make_shared<sync::Chan<int, 1>>();
    ch_buf->TryWrite(0);

    bbt::core::thread::CountDownLatch latch{2};
    detail::Coroutine* r_co = nullptr;
    bool r_returned = false;

    bbtco[&]()
    {
        r_co = g_bbt_tls_coroutine_co;

        EnqueuePeer([&]() {
            if (r_returned) {
                latch.Down();
                return;
            }
            if (cancel_first)
                r_co->RequestCancel();
            if (timeout_ms > 0) {
                std::mutex m;
                std::unique_lock<std::mutex> lock(m);
                std::condition_variable cv;
                cv.wait_for(lock, std::chrono::milliseconds(timeout_ms + 100), []() { return false; });
            }
            r.zb_probe = ch_zb->TryWrite(42);
            int v = 0;
            r.drain = ch_buf->TryRead(v);
            latch.Down();
        });

        int out = 0;
        sync::CoSelect sel;
        sel.CaseRead(*ch_zb, out).CaseWrite(*ch_buf, 99);
        if (!cancel_first)
            sel.CaseTimeout(timeout_ms);
        r.run_ret = sel.Run();
        r_returned = true;
        latch.Down();
    };

    latch.Wait();
    int v = 0;
    r.final_read = ch_buf->TryRead(v);
    return r;
}

void CheckFirstWin(const RaceResult& r)
{
    BOOST_CHECK_EQUAL(r.run_ret, -1);
    BOOST_CHECK_EQUAL(r.zb_probe, -2);
    BOOST_CHECK_EQUAL(r.drain, 0);
    BOOST_CHECK_EQUAL(r.final_read, -2);
}

} // namespace

/**
 * 读 select 与写 select 互补配对，双方都以非零下标取胜（首分支永不就绪，
 * 排除"只对下标 0 生效"），值恰好交付一次，通道无残留；不给超时，永久
 * 挂起即失败。抓错：互补 select 只注册观察者不入队可配对状态，或交付重复。
 */
BOOST_AUTO_TEST_CASE(t_select_zero_complementary_selects_pair)
{
    EnsureRuntime();

    auto ch = std::make_shared<sync::Chan<int, 0>>();
    auto r_never = std::make_shared<sync::Chan<int, 0>>();
    auto w_never = std::make_shared<sync::Chan<int, 0>>();
    bbt::core::thread::CountDownLatch latch{2};
    int read_idx = -999, read_value = 0, write_idx = -999;

    bbtco[&]()
    {
        int out = 0, filler_r = 0;
        read_idx = sync::CoSelect().CaseRead(*r_never, filler_r).CaseRead(*ch, out).Run();
        read_value = out;
        latch.Down();
    };
    bbtco[&]()
    {
        write_idx = sync::CoSelect().CaseWrite(*w_never, 0).CaseWrite(*ch, 42).Run();
        latch.Down();
    };

    latch.Wait();
    BOOST_CHECK_EQUAL(read_idx, 1);
    BOOST_CHECK_EQUAL(read_value, 42);
    BOOST_CHECK_EQUAL(write_idx, 1);
    int v = 0;
    BOOST_CHECK_EQUAL(ch->TryRead(v), -2);
}

/**
 * 零缓冲 select 分支与普通阻塞读写走同一交接，两个方向都成功交付且无残留。
 * 抓错：select 分支与普通读写者用不同等待表达而互不匹配，永久挂起。
 */
BOOST_AUTO_TEST_CASE(t_select_zero_pairs_plain_read_write)
{
    EnsureRuntime();

    struct {
        bool select_reads;
        int write_val;
    } cases[] = {{true, 7}, {false, 11}};

    for (const auto& c : cases) {
        BOOST_TEST_CONTEXT("select_reads=" << c.select_reads)
        {
            auto ch = std::make_shared<sync::Chan<int, 0>>();
            bbt::core::thread::CountDownLatch latch{2};
            int sel_idx = -999, plain_ret = -999, delivered = 0;

            bbtco[&]()
            {
                if (c.select_reads) {
                    int out = 0;
                    sel_idx = sync::CoSelect().CaseRead(*ch, out).Run();
                    delivered = out;
                } else
                    sel_idx = sync::CoSelect().CaseWrite(*ch, c.write_val).Run();
                latch.Down();
            };
            bbtco[&]()
            {
                if (c.select_reads)
                    plain_ret = ch->Write(c.write_val);
                else {
                    int out = 0;
                    plain_ret = ch->Read(out);
                    delivered = out;
                }
                latch.Down();
            };

            latch.Wait();
            BOOST_CHECK_EQUAL(sel_idx, 0);
            BOOST_CHECK_EQUAL(plain_ret, 0);
            BOOST_CHECK_EQUAL(delivered, c.write_val);
            int v = 0;
            BOOST_CHECK_EQUAL(ch->TryRead(v), -2);
        }
    }
}

/**
 * 同一 Run 的读写分支不自配：见证 TryWrite 被调度时 R 已 park 或返回——
 * park 而不返回本身就排除登记时自配；若自配，R 早已带着自己的 5 返回，
 * 见证无人可配，断言落空。读分支应与外部写配对（返回 0、取得 17）。
 */
BOOST_AUTO_TEST_CASE(t_select_zero_no_self_pairing_within_run)
{
    EnsureRuntime();

    auto ch = std::make_shared<sync::Chan<int, 0>>();
    bbt::core::thread::CountDownLatch latch{2};
    int read_idx = -999, read_value = 0, probe_ret = -999;

    bbtco[&]()
    {
        EnqueuePeer([&]() {
            probe_ret = ch->TryWrite(17);
            latch.Down();
        });

        int out = 0;
        read_idx = sync::CoSelect().CaseRead(*ch, out).CaseWrite(*ch, 5).Run();
        read_value = out;
        latch.Down();
    };

    latch.Wait();
    BOOST_CHECK_EQUAL(read_idx, 0);
    BOOST_CHECK_EQUAL(read_value, 17);
    BOOST_CHECK_EQUAL(probe_ret, 0);
    int v = 0;
    BOOST_CHECK_EQUAL(ch->TryRead(v), -2);
}

/**
 * 交付发生在恢复后的读协程上：out 赋值异常被读方自己的 catch 捕获，Run
 * 不返回下标，配对写者收到交付失败（-2）；失败元素不重回通道，后续读者
 * 读不到任何值。取胜分支为非零下标。抓错：交付在配对写者执行流里完成、
 * 读方 catch 不到；写者配对即报成功；落选登记或失败交接留下可配对残留。
 */
BOOST_AUTO_TEST_CASE(t_select_zero_assign_throw_caught_by_reader)
{
    EnsureRuntime();

    auto ch = std::make_shared<sync::Chan<UboThrowOnAssign, 0>>();
    auto ch_never = std::make_shared<sync::Chan<UboThrowOnAssign, 0>>();
    bbt::core::thread::CountDownLatch latch{2};
    int read_idx = -999, read_caught = 0, write_ret = -999;

    UboThrowOnAssign dummy;
    bbtco[&]()
    {
        EnqueuePeer([&]() {
            write_ret = ch->Write(UboThrowOnAssign(42));
            latch.Down();
        });

        UboThrowOnAssign out;
        try {
            read_idx = sync::CoSelect().CaseRead(*ch_never, dummy).CaseRead(*ch, out).Run();
        } catch (const std::runtime_error&) {
            read_caught = 1;
        }
        latch.Down();
    };

    latch.Wait();
    BOOST_CHECK_EQUAL(read_caught, 1);
    BOOST_CHECK_EQUAL(read_idx, -999);
    BOOST_CHECK_EQUAL(write_ret, -2);
    UboThrowOnAssign probe;
    BOOST_CHECK_EQUAL(ch->TryRead(probe), -2);
}

/**
 * 取消先胜：挂起后被取消的读 select 不得再被迟到写配对，恢复后 Run 收口
 * -1，且不得利用见证排空的容量把 99 写进缓冲。
 */
BOOST_AUTO_TEST_CASE(t_select_zero_cancel_first_win_late_writer_fails)
{
    EnsureRuntime();
    CheckFirstWin(RunFirstWinRace(true, 0));
}

/**
 * 超时先胜（截止时间已过）：同上，逾期首胜由截止时间仲裁，不依赖定时器
 * 投递时点。抓错：首胜后迟到写仍与已死等待配对；恢复后重跑就绪分支写 99。
 */
BOOST_AUTO_TEST_CASE(t_select_zero_timeout_first_win_late_writer_fails)
{
    EnsureRuntime();
    CheckFirstWin(RunFirstWinRace(false, 60));
}

BOOST_AUTO_TEST_CASE(t_end) {}

BOOST_AUTO_TEST_SUITE_END()
