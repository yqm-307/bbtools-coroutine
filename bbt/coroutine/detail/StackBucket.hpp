/**
 * @file StackBucket.hpp
 * @brief 栈池中单个 (可用字节数, 保护属性) 规格的空闲缓存与计数。
 */
#pragma once
#include <cstddef>
#include <vector>
#include <bbt/coroutine/detail/Stack.hpp>

namespace bbt::coroutine::detail {

/**
 * @brief 单个尺寸/属性桶：缓存本规格的空闲栈，并记录本规格的对象计数。
 *
 * 发布后的全部访问由所属 StackPool 的池级 mutex 串行化，所以这里不维护并发
 * 队列和原子计数；计数的一致性也由 StackPool 在锁内维护。析构时销毁缓存中的
 * 空闲栈（含 StackPool 构造中途抛出的展开路径）；借出的栈不在缓存里，不受影响。
 */
struct StackBucket {

    /**
     * @brief capacity 只是缓存容器的预留大小，不是缓存数量上限。
     */
    explicit StackBucket(size_t capacity);

    ~StackBucket();
    StackBucket(const StackBucket&) = delete;
    StackBucket& operator=(const StackBucket&) = delete;

    /**
     * @brief 放入一个空闲栈；LIFO，优先复用刚归还的栈。
     * @return 容器扩容失败时返回 false，栈仍归调用方处理。
     */
    bool Push(Stack* item) noexcept;

    /**
     * @brief 取出最近放入的空闲栈；缓存为空时返回 false。
     */
    bool Pop(Stack*& item) noexcept;

    std::vector<Stack*> pool;

    /**
     * @brief 本规格已建成的栈对象数（idle + borrowed）。
     */
    size_t live{0};

    /**
     * @brief 缓存中可复用的空闲栈数。
     */
    size_t idle{0};

    /**
     * @brief 已借出给调用方的栈数。
     */
    size_t borrowed{0};

    /**
     * @brief 已预留容量、分配尚未完成的栈数。
     */
    size_t pending{0};
};

}
