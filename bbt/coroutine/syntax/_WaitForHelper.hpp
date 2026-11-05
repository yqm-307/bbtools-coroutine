/** @file _WaitForHelper.hpp @brief bbtco_wait_for 宏的等待帮手：挂起动作放在构造函数里。 */
#pragma once
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine {

/**
 * @brief 等 fd 事件的等待帮手：挂起动作在构造函数里，所以 `bbtco_wait_for(fd, event, ms);` 是一条语句，
 *        不能出现在表达式中间；必须在协程内调用，失败或等待被取消时抛 std::runtime_error。
 */
class _WaitForHelper final : public std::enable_shared_from_this<_WaitForHelper> {
public:
    /// @brief 挂起当前协程直到 fd 就绪或超时；不在协程内断言失败，等待失败抛 std::runtime_error
    explicit _WaitForHelper(int fd, short event, int ms);
    /// @brief 无副作用：挂起与唤醒都已在构造里完成
    virtual ~_WaitForHelper() = default;
};

};
