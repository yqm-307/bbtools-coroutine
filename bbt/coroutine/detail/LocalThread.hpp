#pragma once
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

/**
 * @brief 线程局部数据辅助类
 */
class LocalThread
{
public:
    typedef std::unique_ptr<LocalThread> UPtr;

    /**
     * @brief 获取当前线程局部存储实例
     * 
     * @return UPtr& 
     */
    static UPtr& GetTLSInst();

    /**
     * @brief 获取当前线程的Processer
     * 
     * @return std::shared_ptr<Processer> 如果为空说明不允许运行协程 
     */
    std::shared_ptr<Processer> GetProcesser();

    /**
     * @brief 设置当前线程是可以运行协程的
     * 
     * @param enable_use_coroutine 
     */
    void SetEnableUseCo(bool enable_use_coroutine);

    /**
     * @brief 当前线程是否可以运行协程
     * 
     * @return true 
     * @return false 
     */
    bool EnableUseCo();
private:
    volatile bool               m_enable_use_co{false};

};

}