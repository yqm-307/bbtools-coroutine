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

ProcessResult Run(const char* mode, int variant = 0) {
    int channel[2];
    BOOST_REQUIRE_EQUAL(pipe(channel), 0);
    const auto executable = std::string(BBT_ENTRY_CHILD_BASE) + std::to_string(variant);
    const auto child = fork();
    BOOST_REQUIRE(child >= 0);
    if (child == 0) {
        // 硬超时跨 exec 保留；永久等待表现为 SIGALRM，不会被当作预期非零码。
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

std::size_t Count(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size()) {
        ++count;
    }
    return count;
}

ProcessResult Check(const char* mode, bbt::coroutine::RuntimeMain::Terminal terminal,
                    int code, int main_calls, int hook_calls, int factory_calls = 1) {
    const auto result = Run(mode);
    BOOST_CHECK_EQUAL(result.exit_code, code);
    const auto expected = "DONE terminal=" + std::to_string(static_cast<int>(terminal)) +
                          " code=" + std::to_string(code) + " main=" + std::to_string(main_calls) +
                          " hook=" + std::to_string(hook_calls) + " factory=" + std::to_string(factory_calls);
    BOOST_CHECK_MESSAGE(result.output.find(expected) != std::string::npos, expected);
    BOOST_CHECK_EQUAL(Count(result.output, "DONE "), 1u);
    BOOST_CHECK_EQUAL(Count(result.output, "MAIN\n"), main_calls);
    BOOST_CHECK_EQUAL(Count(result.output, "HOOK\n"), hook_calls);
    BOOST_CHECK_EQUAL(Count(result.output, "FACTORY\n"), factory_calls);
    return result;
}
using Terminal = bbt::coroutine::RuntimeMain::Terminal;
} // namespace

BOOST_AUTO_TEST_SUITE(runtime_main)

BOOST_AUTO_TEST_CASE(normal) {
    const auto result = Check("normal", Terminal::MainReturned, 37, 1, 1);
    BOOST_CHECK(result.output.find("FACTORY\n") < result.output.find("HOOK\n"));
    BOOST_CHECK(result.output.find("HOOK\n") < result.output.find("MAIN\n"));
    BOOST_CHECK(result.output.find("MAIN_RETURN\n") < result.output.find("DONE "));
    Check("zero", Terminal::MainReturned, 0, 1, 1);
    Check("default", Terminal::MainReturned, 37, 1, 0, 0);
}

BOOST_AUTO_TEST_CASE(hook_failure) {
    Check("hook_rejected", Terminal::HookRejected, 23, 0, 1);
    Check("hook_threw", Terminal::HookThrew, 1, 0, 1);
}

BOOST_AUTO_TEST_CASE(start_failure) {
    Check("start_failed", Terminal::StartFailed, 1, 0, 0);
    Check("record_oom", Terminal::StartFailed, 1, 0, 0, 0);
    Check("already_started", Terminal::StartFailed, 1, 0, 0, 0);
    Check("null_task", Terminal::StartFailed, 1, 0, 0, 0);
}

BOOST_AUTO_TEST_CASE(submit_failure) {
    Check("submit_failed", Terminal::SubmitFailed, 1, 0, 1);
    Check("submit_stack_failed", Terminal::SubmitFailed, 1, 0, 1);
}

BOOST_AUTO_TEST_CASE(main_failure) {
    Check("main_threw", Terminal::MainThrew, 1, 1, 1);
    Check("main_threw_nonstd", Terminal::MainThrew, 1, 1, 1);
}

BOOST_AUTO_TEST_CASE(options) {
    Check("unprotected", Terminal::MainReturned, 37, 1, 1);
}

BOOST_AUTO_TEST_CASE(invalid_options) {
    for (const auto* mode : {"factory_threw", "zero_workers", "huge_workers", "zero_stack", "overflow_stack", "bounds", "invalid_budget"}) {
        Check(mode, Terminal::StartFailed, 1, 0, 0);
    }
}

BOOST_AUTO_TEST_CASE(stack_policy) {
    Check("strict", Terminal::MainReturned, 37, 1, 1);
    Check("auto", Terminal::MainReturned, 37, 1, 1);
}

BOOST_AUTO_TEST_CASE(reentry) {
    for (const auto* mode : {"hook_reentry", "main_reentry", "repeat"}) {
        const auto result = Check(mode, Terminal::MainReturned, 37, 1, 1);
        BOOST_CHECK_EQUAL(Count(result.output, "REENTRY_REJECTED\n"), 1u);
    }
}

BOOST_AUTO_TEST_CASE(detached) {
    const auto result = Check("detached", Terminal::MainReturned, 37, 1, 1);
    BOOST_CHECK_EQUAL(Count(result.output, "DETACHED_ENTERED\n"), 1u);
    BOOST_CHECK_EQUAL(Count(result.output, "DETACHED_FINISHED\n"), 0u);
}

BOOST_AUTO_TEST_CASE(macros) {
    for (int variant = 1; variant <= 5; ++variant) {
        const auto result = Run("normal", variant);
        BOOST_CHECK_EQUAL(result.exit_code, 37);
        BOOST_CHECK_EQUAL(Count(result.output, "MAIN_RETURN\n"), 1u);
        BOOST_CHECK_EQUAL(Count(result.output, "HOOK\n"), variant == 2 || variant == 4 || variant == 5 ? 1u : 0u);
        BOOST_CHECK_EQUAL(Count(result.output, "FACTORY\n"), variant >= 3 ? 1u : 0u);
        BOOST_CHECK_EQUAL(Count(result.output, "SETUP\n"), variant == 5 ? 1u : 0u);
    }
    BOOST_CHECK_EQUAL(Run("hook_threw", 2).exit_code, 1);
    BOOST_CHECK_EQUAL(Run("factory_threw", 3).exit_code, 1);
    BOOST_CHECK_EQUAL(Run("main_threw_nonstd", 1).exit_code, 1);
}

BOOST_AUTO_TEST_SUITE_END()
