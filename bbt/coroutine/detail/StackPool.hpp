#pragma once
#include <queue>
#include <mutex>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Stack.hpp>

namespace bbt::coroutine::detail
{

class StackPool
{
public:
    typedef std::unique_ptr<StackPool>  UPtr;
    typedef Stack                       ItemType;

    static UPtr&                        GetInstance();
    StackPool();
    ~StackPool();

    void                                Release(ItemType* item);
    ItemType*                           Apply();
    int                                 AllocSize();

    // TODO后续做定时检测并释放
    void                                OnUpdate() {}
private:
    std::queue<ItemType*>               m_pool;
    std::mutex                          m_pool_mutex;

    uint32_t                            m_alloc_obj_count{0};
    const int                           m_max_size{30000};
};

}