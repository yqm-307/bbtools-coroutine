#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/base/Attribute.hpp>

namespace bbt::coroutine::sync
{

class CoCond:
    public std::enable_shared_from_this<CoCond>
{
public:
    typedef std::shared_ptr<CoCond> SPtr;
    static SPtr                 Create();

    BBTATTR_FUNC_Ctor_Hidden    CoCond();
                                ~CoCond();

    int                         Wait();

    /**
     * @brief 等待并伴随一个超时时间
     * 
     * @param ms 
     * @return int 
     */
    // int                         WaitWithTimeout(int ms);

    /**
     * @brief 唤醒一个Wait中的协程
     * 
     * @return  0表示成功，-1表示失败
     */
    int                         Notify();
protected:
    int                         Init();
protected:
    // int                         m_pipe_fds[2];
    std::shared_ptr<detail::CoPollEvent>    m_co_event{nullptr};
    std::mutex                      m_co_event_mutex;
};

}