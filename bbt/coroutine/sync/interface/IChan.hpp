#pragma once
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::sync
{

template<class _TItem>
class IChan
{
public:
    typedef _TItem ItemType;

    /**
     * @brief 向信道中写入对象，如果没有空间，阻塞直到可写
     * 
     * @param item 写入信道的对象
     * @return 0表示成功；-1失败 
     */
    virtual int Write(const ItemType& item) = 0;

    /**
     * @brief 从信道读一个对象
     * 
     * @param item 读取对象
     * @return 0表示成功；-1失败 
     */
    virtual int Read(ItemType& item) = 0;

    /**
     * @brief 尝试读一个对象，无法立即读取到返回
     * 
     * @param item 读取对象
     * @return 0表示成功；-1读取失败
     */
    virtual int TryRead(ItemType& item) = 0;

    /**
     * @brief 尝试读取一个对象，尝试直到超时返回
     * 
     * @param item 读取对象
     * @param timeout 超时(毫秒)
     * @return 0表示成功；-1读取失败
     */
    virtual int TryRead(ItemType& item, int timeout) = 0;

    /**
     * @brief 尝试写，立即返回
     * 
     * @param item 写入对象
     * @return int 0表示成功；-1失败
     */
    virtual int TryWrite(const ItemType& item) = 0;

    /**
     * @brief 尝试写，直到超时返回
     * 
     * @param item 写入对象
     * @param timeout 超时时间
     * @return int 0表示成功；-1失败
     */
    virtual int TryWrite(const ItemType& item, int timeout) = 0;

    /**
     * @brief 读取所有值
     * 
     * @param item 
     * @return int 
     */
    virtual int ReadAll(std::vector<ItemType>& item) = 0;

    /**
     * @brief 关闭当前信道
     * 
     */
    virtual void Close() = 0;

    /**
     * @brief 是否已经关闭
     * 
     * @return true 
     * @return false 
     */
    virtual bool IsClosed() = 0;
};

}
