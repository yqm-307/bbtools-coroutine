/**
 * @file RuntimeOptions.hpp
 * @brief runtime 启动前的值快照；不持有运行时或网络资源。
 */
#pragma once
#include <cstddef>
#include <optional>
#include <vector>

namespace bbt::coroutine {

/**
 * @brief 页对齐后的精确可用容量与保护属性；0 字节非法。
 */
struct StackRequest {
    std::size_t usable_bytes;
    bool protect;
};

/**
 * @brief 固定尺寸类预算或按请求建立精确桶的预算。
 */
enum class StackPolicy { Strict, Auto };

/**
 * @brief 一个精确尺寸类可接纳的栈对象上限。
 */
struct StackClassLimit {
    StackRequest request;
    std::size_t max_objects;
};

/**
 * @brief 栈策略值输入；Strict 与 Auto 的预算字段不能混用。
 */
struct StackConfig {
    StackPolicy policy;
    std::vector<StackClassLimit> strict_classes;
    std::size_t auto_max_objects;
    std::size_t auto_max_usable_bytes;
};

/**
 * @brief Start 前一次性消费的三类便捷配置；未指定字段沿用 setup_runtime 的值。
 *
 * scheduler 的其余全局配置由 setup_runtime() 在本类型消费前设置。
 * Start 后不得通过 hook 或主任务修改底层 GlobalConfig。
 * stacks 省略时为 Auto：全局协程对象上限与 256 MiB 可用字节预算；显式策略完整校验后生效。
 */
struct RuntimeOptions {
    /**
     * 有值时必须 >= 1，并满足既有 min/max 边界。
     */
    std::optional<std::size_t> initial_workers;

    /**
     * 主任务与省略尺寸的内部任务使用的默认栈。
     */
    std::optional<StackRequest> default_stack;
    std::optional<StackConfig> stacks;
};

}
