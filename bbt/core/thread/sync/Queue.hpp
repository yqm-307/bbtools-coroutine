/**
 * @file Queue.hpp
 * @brief boost::lockfree::queue 的薄包装，附带一个仅供参考的元素计数。
 */
#pragma once
#include <boost/lockfree/queue.hpp>
#include <boost/noncopyable.hpp>

namespace bbt::core::thread {

/**
 * @brief 多生产者/多消费者无锁队列包装：push/pop 本身无锁，构造与析构仍须与其他线程同步。
 * m_count 在底层操作成功之后才加减，两步不原子，Size() 只是近似值，判断有无元素只能看 Pop 的返回值。
 * TValue 需可拷贝/可赋值（push 按值拷入节点，pop 通过赋值搬出）。
 */
template <typename TValue, typename... Options> class Queue : public boost::noncopyable {
public:
    /**
     * @brief 预分配 @p default_size 个 freelist 节点后构造。
     * @p default_size 不是容量上限：池耗尽时 push 仍会向 OS 申请节点（这一步可能不是无锁的）。
     */
    Queue(size_t default_size);

    /**
     * @brief 不预分配；仅在模板参数指定了 boost::lockfree::capacity 时可用。
     */
    Queue() = default;

    /**
     * @brief 析构：销毁底层队列，不得与其他线程的 push/pop 并发。
     */
    ~Queue() = default;

    /**
     * @brief 入队一个元素。
     * @return true 已入队；false 表示节点分配失败（指定了 capacity 时即队列已满），元素未入队。
     */
    bool Push(const TValue& cmd);

    /**
     * @brief 出队一个元素。
     * @return true 取出并写入 @p cmd；false 表示队列为空，且此时 @p cmd 可能已被 boost 改写，不要读。
     */
    bool Pop(TValue& cmd);

    /**
     * @brief 是否为空；有并发修改时不准确，不能作为程序逻辑的判据。
     */
    bool Empty() const;

    /**
     * @brief 元素个数近似值；size_t 转 int 读出，超过 int 上限会变负，此时按 0 返回。
     */
    size_t Size() const;

private:
    boost::lockfree::queue<TValue, Options...> m_queue;

    /**
     * 成功 push/pop 后加减，与底层队列不同步，只供 Size() 参考
     */
    std::atomic_size_t m_count{0};
};

}

#include <bbt/core/thread/sync/__TQueue.hpp>
