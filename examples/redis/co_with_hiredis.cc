#include <hiredis/hiredis.h>
#include <atomic>
#include <random>
#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

/**
 * 此示例和构建方法用来展示我们如何hook一个第三方库。并使用协程
 * 
 * 这个例子中有几个注意点：
 * 1、构建时需要确保 hiredis 在 bbt_coroutine 之后链接，这样才能正确覆盖系统调用。（参考examples/CMakeLists.txt）；
 * 2、使用同步接口无损的转化为协程接口。并且不会导致线程的阻塞；
 * 3、使用CoPool来处理任务；
 */

class RedisClient
{
public:
    RedisClient(const std::string& host, int port):
        m_context(redisConnect(host.c_str(), port))
    {
        if (m_context == nullptr || m_context->err) {
            throw std::runtime_error("Could not connect to Redis server");
        }
    }

    ~RedisClient()
    {
        if (m_context) {
            redisFree(m_context);
        }
    }

    std::string Get(const std::string& key)
    {
        redisReply* reply = static_cast<redisReply*>(redisCommand(m_context, "GET %s", key.c_str()));
        if (reply == nullptr)
            throw std::runtime_error("Failed to get reply from Redis server");

        if (reply->type == REDIS_REPLY_ERROR) {
            std::string error(reply->str, reply->len);
            freeReplyObject(reply);
            throw std::runtime_error("Redis error: " + error);
        }

        if (reply->type == REDIS_REPLY_STRING) {
            std::string value(reply->str, reply->len);
            freeReplyObject(reply);
            return value;
        }

        freeReplyObject(reply);
        throw std::runtime_error("Unexpected reply type from Redis server");
    }

    void Set(const std::string& key, const std::string& value)
    {
        redisReply* reply = static_cast<redisReply*>(redisCommand(m_context, "SET %s %s", key.c_str(), value.c_str()));
        if (reply == nullptr)
            throw std::runtime_error("Failed to get reply from Redis server");

        if (reply->type == REDIS_REPLY_ERROR) {
            std::string error(reply->str, reply->len);
            freeReplyObject(reply);
            throw std::runtime_error("Redis error: " + error);
        }

        if (reply->type == REDIS_REPLY_STATUS && strcmp(reply->str, "OK") == 0) {
            freeReplyObject(reply);
            return;
        }

        const int reply_type = reply->type;
        freeReplyObject(reply);
        throw std::runtime_error("Unexpected reply type from Redis server, Reply type: " + std::to_string(reply_type));
    }

    bool Exists(const std::string& key)
    {
        redisReply* reply = static_cast<redisReply*>(redisCommand(m_context, "EXISTS %s", key.c_str()));
        if (reply == nullptr)
            throw std::runtime_error("Failed to get EXISTS reply from Redis server");
        if (reply->type != REDIS_REPLY_INTEGER) {
            const int reply_type = reply->type;
            freeReplyObject(reply);
            throw std::runtime_error("Unexpected EXISTS reply type: " + std::to_string(reply_type));
        }
        const bool exists = reply->integer != 0;
        freeReplyObject(reply);
        return exists;
    }

    void Delete(const std::string& key)
    {
        redisReply* reply = static_cast<redisReply*>(redisCommand(m_context, "DEL %s", key.c_str()));
        if (reply == nullptr)
            throw std::runtime_error("Failed to get DEL reply from Redis server");
        if (reply->type != REDIS_REPLY_INTEGER) {
            const int reply_type = reply->type;
            freeReplyObject(reply);
            throw std::runtime_error("Unexpected DEL reply type: " + std::to_string(reply_type));
        }
        freeReplyObject(reply);
    }

    void Run() {
        while (m_is_running) { sleep(1); };
    };

    void Stop() {
        m_is_running = false;
    }

private:
    redisContext* m_context{nullptr};
    volatile bool m_is_running{true};
};

/**
 * 简单介绍下载hiredis中使用协程的方式
 */
void Example1()
{
    RedisClient client("127.0.0.1", 6379);

    // 这里可以设置同步操作
    client.Set("key1", "value1");
    std::string value = client.Get("key1");
    std::cout << "Value for key1: " << value << std::endl;

    // 同时我们可以直接使用hiredis的同步接口在一个hiredis的同步上下文中
    // 同时因为hiredis的同步接口是阻塞的，所以我们可以在协程中使用
    // 注意：hiredis的redisContext不是线程安全的
    bbtco [&](){
        try {
            client.Set("key2", "value2");
            std::string value2 = client.Get("key2");
            std::cout << "Value for key2: " << value2 << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }

        client.Stop();
    };


    client.Run();
}

/**
 * 使用协程池处理并发任务
 */
void Example2()
{
    auto copool = bbtco_make_copool(10);
    constexpr int kWorkers = 10;
    constexpr int kOperationsPerWorker = 1000;
    std::atomic_int completed_workers{0};
    std::atomic_int success_count{0};
    std::atomic_int error_count{0};

    for (int worker = 0; worker < kWorkers; ++worker)
    {
        copool->Submit([&, worker]() {
            try {
                RedisClient client("127.0.0.1", 6379);
                std::mt19937 rng(0x6d315f07u + static_cast<unsigned>(worker));
                for (int i = 0; i < kOperationsPerWorker; ++i) {
                    const int operation_id = worker * kOperationsPerWorker + i;
                    const std::string key = "m1:acceptance:" + std::to_string(operation_id);
                    const size_t value_len = 1 + (rng() % 256);
                    std::string value(value_len, 'a');
                    for (char& ch : value)
                        ch = static_cast<char>(' ' + (rng() % 95));

                    client.Set(key, value);
                    std::string actual = client.Get(key);
                    if (actual != value || !client.Exists(key))
                        error_count++;
                    else
                        success_count++;

                    if ((rng() % 3) == 0) {
                        client.Set(key, "");
                        if (client.Get(key) != "")
                            error_count++;
                    }
                    if ((rng() % 4) == 0) {
                        client.Delete(key);
                        if (client.Exists(key))
                            error_count++;
                    }
                }

                completed_workers.fetch_add(1, std::memory_order_release);
            } catch (const std::exception& e) {
                error_count++;
                completed_workers.fetch_add(1, std::memory_order_release);
                std::cerr << "Error: " << e.what() << std::endl;
            }
        });
    }

    while (completed_workers.load(std::memory_order_acquire) < kWorkers)
        bbtco_sleep(1);
    std::cout << "Total successful operations: " << success_count.load() << std::endl;
    std::cout << "Total errors: " << error_count.load() << std::endl;
}

/** hiredis 在协程栈上有较多临时缓冲：默认栈 1 MiB，全池对象预算 1000。 */
RuntimeOptions HiredisOptions() {
    RuntimeOptions options;
    options.default_stack = StackRequest{1024 * 1024, true};
    options.stacks = StackConfig{StackPolicy::Auto, {}, 1000, 256 * 1024 * 1024};
    return options;
}

int bbtco_main() {
    Example1();
    Example2();

    /* process-lifetime：无业务停机入口；业务完成后返回 main 即进程退出 */
    return 0;
}

BBTCO_MAIN_WITH_OPTIONS(HiredisOptions())