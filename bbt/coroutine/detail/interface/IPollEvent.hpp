namespace bbt::coroutine::detail
{
class IPoller;

class IPollEvent
{
public:
    /**
     * @brief 获取套接字
     * 
     * @return int 
     */
    virtual int GetFd() = 0;

    /**
     * @brief 触发事件
     * 
     * @param poller 触发事件的轮询器
     * @param trigger_events 触发的事件类型
     */
    virtual void Trigger(IPoller* poller, int trigger_events) = 0;

    virtual int GetEvent() = 0;
};

}