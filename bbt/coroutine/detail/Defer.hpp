/**
 * 协程作用域退出执行器 Defer（bbtco_defer 底层）。
 */
#pragma once
#include <boost/noncopyable.hpp>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail {

/**
 * @brief 作用域退出时执行一次回调的 RAII 对象（bbtco_defer 的实现）；不可复制。
 * 回调在退出所在线程同步执行且必须不抛出：析构没有 catch，异常会从析构逃出，若正处在栈展开中则 terminate。
 */
class Defer final : boost::noncopyable {
public:
    /**
     * @brief 保存回调；空回调表示退出时什么都不做。
     */
    explicit Defer(const DeferCallback& cb);
    ~Defer();

private:
    DeferCallback m_defer_handler{nullptr};
};

}