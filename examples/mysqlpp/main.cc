#include <cstdio>
#include <cstdlib>
#include <memory>

#include <mysql/mysql.h>

#include <bbt/coroutine/coroutine.hpp>

int bbtco_main_thread_hook()
{
    const char* user = std::getenv("BBT_EXAMPLE_MYSQL_USER");
    const char* password = std::getenv("BBT_EXAMPLE_MYSQL_PASSWORD");
    if (user == nullptr || *user == '\0') {
        std::fputs("set BBT_EXAMPLE_MYSQL_USER for your local test server\n", stderr);
        return 1;
    }

    std::unique_ptr<MYSQL, decltype(&mysql_close)> client(mysql_init(nullptr), mysql_close);
    if (!client) {
        std::fputs("mysql_init failed\n", stderr);
        return 1;
    }
    const unsigned timeout_seconds = 2;
    if (mysql_options(client.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout_seconds) != 0
        || mysql_options(client.get(), MYSQL_OPT_READ_TIMEOUT, &timeout_seconds) != 0
        || mysql_options(client.get(), MYSQL_OPT_WRITE_TIMEOUT, &timeout_seconds) != 0
        || !mysql_real_connect(client.get(), "127.0.0.1", user, password, nullptr, 3306, nullptr, 0)
        || mysql_query(client.get(), "SELECT 1") != 0) {
        std::fprintf(stderr, "MySQL: %s\n", mysql_error(client.get()));
        return 1;
    }
    std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(
        mysql_store_result(client.get()), mysql_free_result);
    const auto row = result ? mysql_fetch_row(result.get()) : nullptr;
    if (row == nullptr || row[0] == nullptr)
        return 1;
    std::printf("native setup: SELECT 1 -> %s\n", row[0]);
    return 0;
}

int bbtco_main()
{
    std::puts("main coroutine started");
    return 0;
}

BBTCO_MAIN_WITH_HOOK()
