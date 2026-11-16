#include <bbt/coroutine/runtime/RuntimeMain.hpp>
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
struct ProcessResult {
    int exit_code;
    std::string output;
};

ProcessResult Run(const char* mode, int variant = 0)
{
    int channel[2];
    BOOST_REQUIRE_EQUAL(pipe(channel), 0);
    const auto executable = std::string(BBT_ENTRY_CHILD_BASE) + std::to_string(variant);
    const auto child = fork();
    BOOST_REQUIRE(child >= 0);
    if (child == 0) {
        /**
         * 硬超时跨 exec 保留；永久等待表现为 SIGALRM，不会被当作预期非零码。
         */
        alarm(10);
        close(channel[0]);
        dup2(channel[1], STDOUT_FILENO);
        dup2(channel[1], STDERR_FILENO);
        close(channel[1]);
        setenv("BBT_ENTRY_CASE", mode, 1);
        execl(executable.c_str(), executable.c_str(), nullptr);
        std::_Exit(127);
    }
    close(channel[1]);
    std::string output;
    char buffer[4096];
    for (;;) {
        const auto size = read(channel[0], buffer, sizeof(buffer));
        if (size > 0) {
            output.append(buffer, static_cast<std::size_t>(size));
        } else if (size < 0 && errno == EINTR) {
            continue;
        } else {
            BOOST_REQUIRE(size == 0);
            break;
        }
    }
    close(channel[0]);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    std::fprintf(stderr, "CHILD mode=%s variant=%d status=%d\n%s", mode, variant, status, output.c_str());
    BOOST_REQUIRE_EQUAL(waited, child);
    BOOST_REQUIRE_MESSAGE(WIFEXITED(status), "child did not return from native main: " << output);
    BOOST_REQUIRE(output.find("PROBE_FAIL") == std::string::npos);
    return {WEXITSTATUS(status), output};
}

std::size_t Count(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size()) {
        ++count;
    }
    return count;
}

ProcessResult Check(const char* mode, bbt::coroutine::RuntimeMain::Terminal terminal, int code, int main_calls,
                    int hook_calls, int factory_calls = 1)
{
    const auto result = Run(mode);
    BOOST_CHECK_EQUAL(result.exit_code, code);
    const auto expected = "DONE terminal=" + std::to_string(static_cast<int>(terminal))
                          + " code=" + std::to_string(code) + " main=" + std::to_string(main_calls)
                          + " hook=" + std::to_string(hook_calls) + " factory=" + std::to_string(factory_calls);
    BOOST_CHECK_MESSAGE(result.output.find(expected) != std::string::npos, expected);
    BOOST_CHECK_EQUAL(Count(result.output, "DONE "), 1u);
    BOOST_CHECK_EQUAL(Count(result.output, "MAIN\n"), main_calls);
    BOOST_CHECK_EQUAL(Count(result.output, "HOOK\n"), hook_calls);
    BOOST_CHECK_EQUAL(Count(result.output, "FACTORY\n"), factory_calls);
    return result;
}

ProcessResult CheckHost(const char* mode, bbt::coroutine::RuntimeMain::Terminal terminal, int code, int main_calls,
                        int host_calls, int callback_calls)
{
    const auto result = Run(mode);
    BOOST_CHECK_EQUAL(result.exit_code, code);
    const auto expected = "DONE terminal=" + std::to_string(static_cast<int>(terminal)) + " code="
                          + std::to_string(code) + " main=" + std::to_string(main_calls) + " hook=0 factory=1";
    BOOST_CHECK_MESSAGE(result.output.find(expected) != std::string::npos, expected);
    BOOST_CHECK_EQUAL(Count(result.output, "DONE "), 1u);
    BOOST_CHECK_EQUAL(Count(result.output, "MAIN\n"), main_calls);
    BOOST_CHECK_EQUAL(Count(result.output, "HOOK\n"), 0u);
    BOOST_CHECK_EQUAL(Count(result.output, "FACTORY\n"), 1u);
    BOOST_CHECK_EQUAL(Count(result.output, "SETUP\n"), 1u);
    BOOST_CHECK_EQUAL(Count(result.output, "HOST_ENTER\n"), host_calls);
    if (callback_calls >= 0)
        BOOST_CHECK_EQUAL(Count(result.output, " cb=" + std::to_string(callback_calls) + "\n"), 1u);
    return result;
}
using Terminal = bbt::coroutine::RuntimeMain::Terminal;
}

BOOST_AUTO_TEST_SUITE(runtime_main)

BOOST_AUTO_TEST_CASE(normal)
{
    const auto result = Check("normal", Terminal::MainReturned, 37, 1, 1);
    BOOST_CHECK(result.output.find("FACTORY\n") < result.output.find("HOOK\n"));
    BOOST_CHECK(result.output.find("HOOK\n") < result.output.find("MAIN\n"));
    BOOST_CHECK(result.output.find("MAIN_RETURN\n") < result.output.find("DONE "));
    Check("zero", Terminal::MainReturned, 0, 1, 1);
    Check("default", Terminal::MainReturned, 37, 1, 0, 0);
}

BOOST_AUTO_TEST_CASE(hook_failure)
{
    Check("hook_rejected", Terminal::HookRejected, 23, 0, 1);
    Check("hook_threw", Terminal::HookThrew, 1, 0, 1);
}

BOOST_AUTO_TEST_CASE(start_failure)
{
    Check("start_failed", Terminal::StartFailed, 1, 0, 0);
    Check("record_oom", Terminal::StartFailed, 1, 0, 0, 0);
    Check("already_started", Terminal::StartFailed, 1, 0, 0, 0);
    Check("null_task", Terminal::StartFailed, 1, 0, 0, 0);
}

BOOST_AUTO_TEST_CASE(submit_failure)
{
    Check("submit_failed", Terminal::SubmitFailed, 1, 0, 1);
    Check("submit_stack_failed", Terminal::SubmitFailed, 1, 0, 1);
}

BOOST_AUTO_TEST_CASE(main_failure)
{
    Check("main_threw", Terminal::MainThrew, 1, 1, 1);
    Check("main_threw_nonstd", Terminal::MainThrew, 1, 1, 1);
}

BOOST_AUTO_TEST_CASE(options)
{
    Check("unprotected", Terminal::MainReturned, 37, 1, 1);
}

BOOST_AUTO_TEST_CASE(invalid_options)
{
    for (const auto* mode : {"factory_threw", "zero_workers", "huge_workers", "zero_stack", "overflow_stack", "bounds",
                             "invalid_budget"}) {
        Check(mode, Terminal::StartFailed, 1, 0, 0);
    }
}

BOOST_AUTO_TEST_CASE(stack_policy)
{
    Check("strict", Terminal::MainReturned, 37, 1, 1);
    Check("auto", Terminal::MainReturned, 37, 1, 1);
}

BOOST_AUTO_TEST_CASE(reentry)
{
    for (const auto* mode : {"hook_reentry", "main_reentry", "repeat"}) {
        const auto result = Check(mode, Terminal::MainReturned, 37, 1, 1);
        BOOST_CHECK_EQUAL(Count(result.output, "REENTRY_REJECTED\n"), 1u);
    }
}

BOOST_AUTO_TEST_CASE(detached)
{
    const auto result = Check("detached", Terminal::MainReturned, 37, 1, 1);
    BOOST_CHECK_EQUAL(Count(result.output, "DETACHED_ENTERED\n"), 1u);
    BOOST_CHECK_EQUAL(Count(result.output, "DETACHED_FINISHED\n"), 0u);
}

BOOST_AUTO_TEST_CASE(macros)
{
    for (int variant = 1; variant <= 7; ++variant) {
        const auto result = Run("normal", variant);
        BOOST_CHECK_EQUAL(result.exit_code, 37);
        BOOST_CHECK_EQUAL(Count(result.output, "MAIN_RETURN\n"), 1u);
        BOOST_CHECK_EQUAL(Count(result.output, "HOOK\n"), variant == 2 || variant == 4 || variant == 5 ? 1u : 0u);
        BOOST_CHECK_EQUAL(Count(result.output, "FACTORY\n"),
                          variant == 3 || variant == 4 || variant == 5 || variant == 7 ? 1u : 0u);
        BOOST_CHECK_EQUAL(Count(result.output, "SETUP\n"), variant == 5 || variant == 6 || variant == 7 ? 1u : 0u);
        BOOST_CHECK_EQUAL(Count(result.output, "HOST_ENTER\n"), variant == 6 || variant == 7 ? 1u : 0u);
    }
    BOOST_CHECK_EQUAL(Run("hook_threw", 2).exit_code, 1);
    BOOST_CHECK_EQUAL(Run("factory_threw", 3).exit_code, 1);
    BOOST_CHECK_EQUAL(Run("main_threw_nonstd", 1).exit_code, 1);
    BOOST_CHECK_EQUAL(Run("host_main_done", 6).exit_code, 21);
    const auto host_options = Run("host_publish", 7);
    BOOST_CHECK_EQUAL(host_options.exit_code, 37);
    BOOST_CHECK_EQUAL(Count(host_options.output, "HOST_ENTER\n"), 1u);
    BOOST_CHECK_EQUAL(Count(host_options.output, "FACTORY\n"), 1u);
}

BOOST_AUTO_TEST_CASE(host_publish)
{
    /**
     * hook 等主任务发布后才继续：提交前执行 hook 会在这里等不到而触发子进程超时。
     */
    const auto result = CheckHost("host_publish", Terminal::MainReturned, 37, 1, 1, 0);
    BOOST_CHECK(result.output.find("MAIN\n") < result.output.find("HOST_RETURN\n"));
    BOOST_CHECK(result.output.find("HOST_RETURN\n") < result.output.find("DONE "));
    BOOST_CHECK(result.output.find("MAIN_RETURN\n") < result.output.find("DONE "));
}

BOOST_AUTO_TEST_CASE(host_main_done)
{
    const auto result = CheckHost("host_main_done", Terminal::MainReturned, 21, 1, 1, 1);
    BOOST_CHECK_EQUAL(Count(result.output, "HOST_CALLBACK_SEEN\n"), 1u);
    BOOST_CHECK(result.output.find("HOST_CALLBACK_SEEN\n") < result.output.find("DONE "));

    /**
     * host hook 活跃期间主任务抛异常：仍先兑现通知，再以 MainThrew 收口。
     */
    const auto threw = CheckHost("host_main_threw", Terminal::MainThrew, 1, 1, 1, 1);
    BOOST_CHECK_EQUAL(Count(threw.output, "HOST_CALLBACK_SEEN\n"), 1u);
}

BOOST_AUTO_TEST_CASE(host_registration)
{
    const auto late = CheckHost("host_late_register", Terminal::MainReturned, 37, 1, 1, 1);
    BOOST_CHECK_EQUAL(Count(late.output, "HOST_LATE_REGISTER\n"), 1u);
    const auto race = CheckHost("host_register_race", Terminal::MainReturned, 37, 1, 1, 1);
    BOOST_CHECK_EQUAL(Count(race.output, "HOST_REGISTERED\n"), 1u);

    /**
     * 单槽替换：两次登记都在主任务完成前提交，只有后一次被执行。
     */
    const auto replace = CheckHost("host_replace", Terminal::MainReturned, 37, 1, 1, 1);
    BOOST_CHECK_EQUAL(Count(replace.output, "HOST_REPLACED\n"), 1u);
    BOOST_CHECK_EQUAL(Count(replace.output, "STALE_RAN\n"), 0u);

    /**
     * 替换与完成发布并发：后登记恰好一次，被替换的旧登记至多一次（不得重复发起）。
     */
    const auto swap = CheckHost("host_replace_race", Terminal::MainReturned, 37, 1, 1, 1);
    BOOST_CHECK_EQUAL(Count(swap.output, "HOST_REPLACED\n"), 1u);
    BOOST_CHECK(Count(swap.output, "STALE_RAN\n") <= 1u);
    BOOST_CHECK(swap.output.find("MAIN_RETURN\n") < swap.output.find("DONE "));

    /**
     * 回调在 runtime 锁外执行：回调内查询完成位并再次登记不得自我死锁。
     */
    const auto reentry = CheckHost("host_callback_reentry", Terminal::MainReturned, 37, 1, 1, 1);
    BOOST_CHECK_EQUAL(Count(reentry.output, "HOST_REENTRY\n"), 1u);
}

BOOST_AUTO_TEST_CASE(host_exit)
{
    const auto exits = CheckHost("host_exits_first", Terminal::MainReturned, 37, 1, 1, -1);
    BOOST_CHECK_EQUAL(Count(exits.output, "REENTRY_REJECTED\n"), 1u);
    BOOST_CHECK(exits.output.find("HOST_RETURN\n") < exits.output.find("MAIN_RETURN\n"));
    BOOST_CHECK(exits.output.find("MAIN_RETURN\n") < exits.output.find("DONE "));
    BOOST_CHECK_EQUAL(Count(exits.output, "cb=0\n") + Count(exits.output, "cb=1\n"), 1u);

    /**
     * 回调在 hook 返回前已开始执行、在 hook 栈析构后才完成；完成记录仍只兑现一次。
     */
    const auto flight = CheckHost("host_callback_in_flight", Terminal::MainReturned, 37, 1, 1, 1);
    BOOST_CHECK_EQUAL(Count(flight.output, "HOST_RETURN\n"), 1u);
    BOOST_CHECK(flight.output.find("HOST_RETURN\n") < flight.output.find("DONE "));
}

BOOST_AUTO_TEST_CASE(host_failure)
{
    /**
     * host hook 失败优先：退出码取 hook 的 23，而不是主任务的 99。
     */
    const auto failed = CheckHost("host_failed", Terminal::HostHookFailed, 23, 1, 1, 0);
    BOOST_CHECK_EQUAL(Count(failed.output, "MAIN_ABORT\n"), 1u);
    const auto threw = CheckHost("host_threw", Terminal::HostHookThrew, 1, 1, 1, -1);
    BOOST_CHECK_EQUAL(Count(threw.output, "MAIN_ABORT\n"), 1u);
    BOOST_CHECK_EQUAL(Count(threw.output, "cb=0\n") + Count(threw.output, "cb=1\n"), 1u);
}

BOOST_AUTO_TEST_SUITE_END()
