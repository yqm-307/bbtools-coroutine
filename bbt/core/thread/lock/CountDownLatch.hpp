#pragma once
#include <bbt/core/thread/lock/Mutex.hpp>

namespace bbt::core::thread
{

class CountDownLatch:
    public boost::noncopyable
{
public:
   	CountDownLatch(int cot);
	~CountDownLatch();
    void Wait();

    /**
     * @brief 等待直到被唤醒或者超时
     * 
     * @param timeout 毫秒
     * @return int 0表示被唤醒，-1表示超时
     */
    int WaitTimeout(int timeout);
    void Down();
    void Reset(int n);
private:
    std::atomic_int m_count;
	Mutex           m_lock;
	pthread_cond_t  m_sem;
};

}