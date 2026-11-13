/**
 * @file _DeferHelper.hpp
 * @brief bbtco_defer 宏的运算符帮手：把回调包成 RAII 的 detail::Defer。
 */
#pragma once
#include <bbt/coroutine/detail/Defer.hpp>

namespace bbt::coroutine {

/**
 * @brief 作用域退出执行器宏的左操作数：`_DeferHelper() - cb` 构造一个 detail::Defer 局部对象。
 *        之后与本对象无关：Defer 析构时执行 cb（return 与异常展开都算），顺序同普通析构。
 */
class _DeferHelper {
public:
    /**
     * @brief 拷贝回调并返回持有它的 Defer，通常直接用来初始化宏里的局部变量
     */
    detail::Defer operator-(const detail::DeferCallback& co_func) { return detail::Defer{co_func}; }
};

}
