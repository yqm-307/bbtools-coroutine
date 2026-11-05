#pragma once
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail {

/** @brief 每线程一份的协程执行域登记，回答「本线程能不能跑协程」；实例由 thread_local 持有到线程退出。
 *  native 线程上的只读查询走 TryGetTLSInst()，否则「查一下是不是协程线程」本身就创建了实例。 */
class LocalThread {
public:
    typedef std::unique_ptr<LocalThread> UPtr;
    ~LocalThread() noexcept;

    /** @brief 取当前线程的实例，没有则创建；返回的 UPtr& 由线程持有，调用方不接管、不移动。 */
    static UPtr& GetTLSInst();

    /* 只读执行域查询不得为 native 线程分配 TLS 辅助对象：没建过实例的线程返回
     * nullptr，按「不在协程执行域」处理。 */
    static LocalThread* TryGetTLSInst() noexcept;

    /** @brief 取当前线程的 Processer；未开启协程执行域时为空。
     *  开启后转调 Processer::GetLocalProcesser()，本地没有时会惰性建一个未登记到 runtime 的实例。 */
    std::shared_ptr<Processer> GetProcesser();

    /** @brief 标记当前线程可以运行协程；只由该线程自己在进入 worker 调度循环前调用（写的是本线程 TLS）。 */
    void SetEnableUseCo(bool enable_use_coroutine);

    /** @brief 当前线程是否在协程执行域内（读本线程自己写过的标记）。 */
    bool EnableUseCo();

private:
    /* 同线程读写；volatile 是既有声明，不承担跨线程同步语义 */
    volatile bool m_enable_use_co{false};
};

}