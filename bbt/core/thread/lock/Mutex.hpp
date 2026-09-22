#pragma once
#include <bbt/core/thread/lock/ILock.hpp>
#include <boost/noncopyable.hpp>

namespace bbt::core::thread
{

class Mutex:
    public boost::noncopyable,
    public IBasicLockable
{
public:
	Mutex();
	virtual ~Mutex();
	virtual void        lock() override;

	virtual void        unlock() override;
	virtual bool 		try_lock() override;
	pthread_mutex_t&    getlock();
private:
    pthread_mutexattr_t m_mutex_attr;
	pthread_mutex_t     m_mutex;
};

}