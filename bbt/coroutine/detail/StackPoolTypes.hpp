/**
 * @file StackPoolTypes.hpp
 * @brief 栈池对外交换的小类型：栈归还凭据、等待失败异常与计数快照。
 */
#pragma once
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <bbt/coroutine/detail/Stack.hpp>
#include <bbt/coroutine/sync/WaitTypes.hpp>

namespace bbt::coroutine::detail {

class StackPool;

/**
 * @brief 栈的唯一归还责任；pool 必须活过全部 lease。
 */
struct StackReturner {
    StackPool* pool{nullptr};
    void operator()(Stack* stack) const noexcept;
};

/**
 * @brief 独占借用一个栈；析构即按 Stack 自身规格归还到来源池。
 */
using StackLease = std::unique_ptr<Stack, StackReturner>;

/**
 * @brief 栈等待失败；保留取消、超时与基础设施失败的区别。
 */
class StackWaitError : public std::runtime_error {
public:
    explicit StackWaitError(WaitStatus status);
    WaitStatus Status() const noexcept;

private:
    WaitStatus m_status;
};

/**
 * @brief 同一把池锁下的精确对象数与可用字节快照，不含保护页。
 */
struct StackPoolSnapshot {
    size_t live{0}, borrowed{0}, idle{0}, pending{0};
    size_t committed_usable_bytes{0}, pending_usable_bytes{0};
};

}
