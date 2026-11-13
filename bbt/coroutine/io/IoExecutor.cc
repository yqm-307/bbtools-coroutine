/**
 * @file IoExecutor.cc
 * @brief 转发 CoPoller 全局 EventLoop 的 executor；本 TU 不创建 context、线程或驱动。
 */
#include <bbt/coroutine/io/IoExecutor.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>

namespace bbt::coroutine::io {

boost::asio::any_io_executor GetExecutor()
{

    /**
     * 与 Scheduler 的 PollOnce 共用同一个 io_context：调用方只能往里投递异步操作，run/poll/stop 不归它管。
     */
    return detail::CoPoller::GetInstance()->GetExecutor();
}

}
