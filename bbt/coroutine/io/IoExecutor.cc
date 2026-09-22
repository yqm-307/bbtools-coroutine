#include <bbt/coroutine/io/IoExecutor.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>

namespace bbt::coroutine::io
{

boost::asio::any_io_executor GetExecutor()
{
    return detail::CoPoller::GetInstance()->GetExecutor();
}

}
