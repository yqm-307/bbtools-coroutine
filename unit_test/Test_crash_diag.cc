#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

// #278 崩溃与栈溢出诊断边界（评估+锁行为）。fork 子进程做破坏性验证，
// 父进程 waitpid 收尸判定信号；不在测试进程内触发 SIGSEGV。

#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(CrashDiagTest)

// 深递归越过 16KB 栈：保护页必须把 SIGSEGV 送到现场（fail-fast 可诊断，
// 不是静默跑飞）。fork 在 scheduler 启动之前，子进程自建运行时——
// 从已 Start 的父进程 fork 不复制调度线程，属运行时未定义。
static int RunStackOverflowChild(bool protect)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = 1;
        cfg->m_cfg_stack_size = 16384;
        cfg->m_cfg_stack_protect = protect;
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);

        volatile char sink[4096];
        std::function<void(int)> rec = [&](int depth) {
            char buf[2048];
            memset(buf, depth & 0xff, sizeof(buf));
            if ((size_t)(uintptr_t)buf & 1) sink[0] = buf[0];
            if (depth > 0) rec(depth - 1);
            if ((size_t)(uintptr_t)buf & 2) sink[0] = buf[1];
        };
        bbt::core::thread::CountDownLatch done{1};
        bbtco [&]() { rec(200); done.Down(); };
        done.WaitTimeout(3);
        _exit(0);   // 正常返回 = 没有崩（保护未命中）
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) ? WTERMSIG(status) : -1;
}

BOOST_AUTO_TEST_CASE(t_protected_stack_overflow_segv)
{
    int sig = RunStackOverflowChild(true);
    BOOST_CHECK_EQUAL(sig, SIGSEGV);
}

// NDEBUG/Debug 断言路径行为（Release=打印不 abort，Debug=abort）由 Assert.hpp
// 宏本身决定，随构建类型翻转，不在本测试锁死；评估结论见 m1-05d 文档。
BOOST_AUTO_TEST_CASE(t_end)
{
}

BOOST_AUTO_TEST_SUITE_END()
