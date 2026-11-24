#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#include <hiredis/hiredis.h>
#include <unistd.h>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

template <class... Args> Reply Command(redisContext* client, int expected_type, const char* format, Args... args)
{
    Reply reply(static_cast<redisReply*>(redisCommand(client, format, args...)), freeReplyObject);
    if (!reply || reply->type != expected_type)
        throw std::runtime_error("unexpected Redis reply");
    return reply;
}

int RoundTrips(int port, int worker)
{
    // 每个任务独占一个 context，不能在多个协程间复用。
    const timeval timeout{2, 0};
    std::unique_ptr<redisContext, decltype(&redisFree)> client(
        redisConnectWithTimeout("127.0.0.1", port, timeout), redisFree);
    if (!client || client->err || redisSetTimeout(client.get(), timeout) != REDIS_OK)
        throw std::runtime_error("could not connect to local Redis");

    const std::string prefix = "bbt-example:" + std::to_string(::getpid()) + ':' + std::to_string(worker) + ':';
    std::mt19937 random(0x6d315f07u + static_cast<unsigned>(worker));
    int successes = 0;
    for (int i = 0; i < 1000; ++i) {
        const auto key = prefix + std::to_string(i);
        std::string value(1 + random() % 256, 'a');
        for (char& ch : value)
            ch = static_cast<char>(' ' + random() % 95);
        // 设置 TTL，即使提前退出也不留永久示例数据。
        Command(client.get(), REDIS_REPLY_STATUS, "SET %s %b EX 60", key.c_str(), value.data(), value.size());
        const auto read = Command(client.get(), REDIS_REPLY_STRING, "GET %s", key.c_str());
        const auto exists = Command(client.get(), REDIS_REPLY_INTEGER, "EXISTS %s", key.c_str());
        if (value != std::string_view(read->str, read->len) || exists->integer != 1)
            throw std::runtime_error("Redis readback mismatch");
        if (random() % 3 == 0) {
            Command(client.get(), REDIS_REPLY_STATUS, "SET %s %b EX 60", key.c_str(), "", std::size_t{0});
            if (Command(client.get(), REDIS_REPLY_STRING, "GET %s", key.c_str())->len != 0)
                throw std::runtime_error("Redis empty value mismatch");
        }
        if (random() % 4 == 0) {
            Command(client.get(), REDIS_REPLY_INTEGER, "DEL %s", key.c_str());
            if (Command(client.get(), REDIS_REPLY_INTEGER, "EXISTS %s", key.c_str())->integer != 0)
                throw std::runtime_error("Redis delete mismatch");
        }
        ++successes;
    }
    return successes;
}

int bbtco_main()
{
    int port = 6379;
    if (const char* setting = std::getenv("BBT_EXAMPLE_REDIS_PORT")) {
        const std::string_view text(setting);
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, port);
        if (parsed.ec != std::errc{} || parsed.ptr != end || port < 1 || port > 65535) {
            std::fputs("invalid BBT_EXAMPLE_REDIS_PORT\n", stderr);
            return 1;
        }
    }

    constexpr int workers = 10;
    auto pool = bbtco_make_copool(workers);
    auto finished = Chan<int, workers>();
    for (int i = 0; i < workers; ++i) {
        if (pool->Submit([port, i, finished]() {
                int successes = -1;
                try {
                    successes = RoundTrips(port, i);
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "Error: %s\n", error.what());
                }
                (void)finished->TryWrite(successes);
            })
            != 0)
            return 1;
    }

    int successes = 0;
    int errors = 0;
    for (int i = 0; i < workers; ++i) {
        int count = 0;
        if (finished->Read(count) != 0)
            return 1;
        if (count < 0)
            ++errors;
        else
            successes += count;
    }
    pool->Release();
    std::printf("Total successful operations: %d\nTotal errors: %d\n", successes, errors);
    return errors == 0 ? 0 : 1;
}

RuntimeOptions Options()
{
    RuntimeOptions options;
    options.default_stack = StackRequest{1024 * 1024, true};
    return options;
}

BBTCO_MAIN_WITH_OPTIONS(Options())
