#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>

using namespace bbt::coroutine;

const char msg[] = "this is a message";

int main()
{
    g_bbt_dbgp_full("这是我的消息");

    g_bbt_dbgp_full(R"(
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    这是我的消息
    )");
    
}