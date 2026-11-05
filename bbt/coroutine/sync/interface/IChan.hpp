/** @file IChan.hpp @brief 信道抽象接口：读写条件不满足时挂起当前协程。 */
#pragma once
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::sync {

/**
 * @brief 信道抽象接口；挂起型接口必须在协程内调用，返回值口径与 Chan 一致。
 */
template <class _TItem> class IChan {
public:
    typedef _TItem ItemType;

    /** @brief 写入对象，无空间时挂起。@return 0 成功；-1 已关闭；-2 等待失败 */
    virtual int Write(const ItemType& item) = 0;

    /** @brief 读一个对象，无数据时挂起。
     *  @return 0 成功；-1 已关闭且无数据；-2 已有读者在消费或等待失败 */
    virtual int Read(ItemType& item) = 0;

    /** @brief 尝试读一个对象，不挂起。@return 0 成功；-1 已关闭；-2 无数据或已有读者在消费 */
    virtual int TryRead(ItemType& item) = 0;

    /** @brief 尝试读一个对象，等待至多 @p timeout 毫秒。
     *  @return 0 成功；1 超时；-1 已关闭；-2 已有读者在消费或等待失败 */
    virtual int TryRead(ItemType& item, int timeout) = 0;

    /** @brief 尝试写，立即返回。@return 0 成功；-1 已关闭；-2 缓冲已满 */
    virtual int TryWrite(const ItemType& item) = 0;

    /** @brief 尝试写，等待至多 @p timeout 毫秒。
     *  @return 0 成功；1 超时；-1 已关闭；-2 等待失败 */
    virtual int TryWrite(const ItemType& item, int timeout) = 0;

    /** @brief 读出当前所有值，追加到 @p item 末尾（不清空）；空且未关闭时挂起。
     *  @return 0 成功；-1 已关闭且无数据；-2 已有读者在消费或等待失败 */
    virtual int ReadAll(std::vector<ItemType>& item) = 0;

    /**
     * @brief 关闭当前信道：唤醒所有等待者，缓冲数据留给读者读完
     */
    virtual void Close() = 0;

    /**
     * @brief 是否已经关闭
     */
    virtual bool IsClosed() = 0;
};

}
