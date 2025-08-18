#include <hiredis/hiredis.h>
#include <bbt/coroutine/coroutine.hpp>

/**
 * 此示例和构建方法用来展示我们如何hook一个第三方库。并使用协程
 * 
 * 这个例子中有几个注意点：
 * 1、构建时需要确保 hiredis 在 bbt_coroutine 之后链接，这样才能正确覆盖系统调用。（参考example/CMakeLists.txt）；
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

        freeReplyObject(reply);
        throw std::runtime_error("Unexpected reply type from Redis server, Reply type: " + std::to_string(reply->type));
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
    RedisClient client("127.0.0.1", 6379);
    auto comutex = bbt::co::sync::StdLockWapper(bbtco_make_comutex());
    auto copool = bbtco_make_copool(10);
    auto wg = bbt::core::thread::CountDownLatch(10000);
    std::atomic_int success_count{0};
    std::atomic_int error_count{0};

    for (int i = 0; i < 10000; ++i)
    {
        copool->Submit([&, i]() {
            std::unique_lock<bbt::co::sync::StdLockWapper> lock(comutex);            
            try {
                client.Set("key" + std::to_string(i), "value" + std::to_string(i));
                std::string value = client.Get("key" + std::to_string(i));
                // std::cout << "Value for key" << i << ": " << value << std::endl;
                if (value != "value" + std::to_string(i))
                    error_count++;
                else
                    success_count++;

                wg.Down();
            } catch (const std::exception& e) {
                error_count++;
                wg.Down();
                std::cerr << "Error: " << e.what() << std::endl;
            }
        });
    }

    wg.Wait();
    std::cout << "Total successful operations: " << success_count.load() << std::endl;
}

int main()
{
    // 初始化，需要更大的栈，因为 hiredis 中使用了大量的栈上内存
    g_bbt_coroutine_config->m_cfg_stack_size = 1024 * 1024; // 设置协程栈大小为1MB
    g_bbt_coroutine_config->m_cfg_max_coroutine = 1000;

    g_scheduler->Start();

    Example1();
    Example2();

    g_scheduler->Stop();

}