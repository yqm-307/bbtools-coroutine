/**
 * @file CoPoolTypes.hpp
 * @brief 尺寸路由池的提交参数与结果类型；CoPool 声明与实现共用。
 */
#pragma once

#include <cstddef>
#include <vector>

#include <bbt/coroutine/runtime/RuntimeOptions.hpp>

namespace bbt::coroutine::pool {

/**
 * @brief 一个尺寸类的不可变配置。
 *
 * request 规范化后作为路由键 (U, protect)：worker 栈规格与提交键必须完全一致，
 * 池不会用相邻尺寸或另一保护属性的 worker 顶替。worker_num 与 queue_capacity
 * 都是有限值，池不会为单个任务临时建栈或无限排队。
 */
struct CoPoolClassConfig {
    StackRequest request{0, false};
    std::size_t worker_num{0};
    std::size_t queue_capacity{0};
};

/**
 * @brief 尺寸路由池的创建参数；各类规范化后的键必须两两不同。
 */
struct CoPoolOptions {
    std::vector<CoPoolClassConfig> classes;
};

/**
 * @brief 尺寸路由提交结果；拒绝原因不合并，调用方可区分重试与用法错误。
 */
enum class CoPoolSubmitStatus {
    Accepted,
    /**
     * 没有精确匹配的 (U, protect) 类；未携带尺寸的提交同样落在这里
     */
    NoExactClass,
    /**
     * 该类有界队列已满
     */
    QueueFull,
    /**
     * 池已封口，不再接纳新任务
     */
    Released,
    /**
     * 任务节点或队列节点分配失败
     */
    AllocationFailed,
};

}
