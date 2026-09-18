#pragma once
#include <boost/asio/any_io_executor.hpp>

namespace bbt::coroutine::io
{

/**
 * @brief 获取 coroutine 运行时共享的 Asio executor
 *
 * 返回当前 CoPoller 全局 EventLoop 所持同一 io_context 的 executor，
 * 供上层适配层（HTTP/Redis/MySQL/Mongo 等）把 Boost.Asio 异步操作
 * 接入既有协程事件循环。投递的 handler 只由 Scheduler 现有
 * PollOnce 驱动执行。
 *
 * 约束：
 *  - 不创建新线程或独立 io_context；executor 仅是投递句柄，
 *    不暴露可 stop/restart/run 的原始 io_context。
 *  - executor 生命周期由全局 EventLoop（CoPoller 单例）保证。
 *  - 使用者须在发起异步操作前自行确认 Scheduler/runtime
 *    generation 可用；调度停止期间投递的 handler 不会被执行。
 */
boost::asio::any_io_executor GetExecutor();

}
