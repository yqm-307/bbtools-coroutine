#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/base/Attribute.hpp>

namespace bbt::coroutine::sync
{

class CoCond:
    public std::enable_shared_from_this<CoCond>
{
public:
    typedef std::unique_ptr<CoCond> UPtr;
    static UPtr Create();

    BBTATTR_FUNC_Ctor_Hidden CoCond();
    ~CoCond();

    int                         Init();
    int                         Wait();
    int                         WaitWithTimeout(int ms);
    int                         Notify();
protected:
    int                         m_fd{-1};
    int                         m_pipe_fds[2];
    void*                       m_data{nullptr};
    std::shared_ptr<detail::CoPollEvent> 
                                m_poller_event;
};

}