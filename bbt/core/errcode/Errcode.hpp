/**
 * @file Errcode.hpp
 * @brief `IErrcode` 的具体实现（一段文本 + 一个错误类型），以及三个常用返回别名。
 */
#pragma once

#include "IErrcode.hpp"
#include <bbt/core/util/Result.hpp>

namespace bbt::core::errcode {

/**
 * @brief 错误码值对象：一段错误文本 + `ErrType`，按值拷贝/移动；不分类、不主动抛出业务错误
 */
class Errcode : public IErrcode {
public:
    /** @brief 构造空错误：类型为 -1（与 `ERR_UNKNOWN` 同值），文本为空串 */
    Errcode();
    /**
     * @brief 用错误文本和类型构造
     * @param type 错误类型，缺省 `ERR_UNKNOWN`
     */
    Errcode(const std::string& msg, ErrType type = ERR_UNKNOWN);
    /** @brief 拷贝构造：文本与类型一起复制，两个对象互不影响 */
    Errcode(const Errcode& other);
    /** @brief 移动构造：源对象的文本被移走，仍可读但内容未指定 */
    Errcode(Errcode&& other);
    /// @brief 析构：释放内部错误文本。
    virtual ~Errcode();

    /** @brief 拷贝赋值：文本与类型整体替换为 other 的 */
    Errcode& operator=(const Errcode& other);
    /** @brief 移动赋值：other 的文本被移走，仍可读但内容未指定 */
    Errcode& operator=(Errcode&& other);

    /** @brief 错误文本；返回内部成员的引用，寿命同对象，改文本不影响引用 */
    virtual const std::string& What() const override;
    /** @brief 错误文本的 C 字符串形式；改文本或析构后指针可能失效，不要缓存 */
    virtual const char* CWhat() const override;
    /** @brief 构造时给定的错误类型 */
    virtual ErrType Type() const override;

protected:
    ErrType m_err_type{-1};    ///< 错误类型，-1 与 `ERR_UNKNOWN` 同值
    std::string m_err_msg{""}; ///< 错误文本
};

/** @brief 「可能失败、没有返回值」的返回：`std::nullopt` 表示成功，有值即错误内容 */
typedef std::optional<bbt::core::errcode::Errcode> ErrOpt;

/** @brief 「错误 + 若干返回值」的返回：第一个元素是 `ErrOpt`，其余是调用结果 */
template <typename... TRetTuple> using ErrTuple = std::tuple<ErrOpt, TRetTuple...>;

/** @brief 「若干返回值 + 错误」的返回：`ErrOpt` 永远放最后，新接口优先用它而不是 `ErrTuple` */
template <typename... TRet> using TupleErr = std::tuple<TRet..., ErrOpt>;

/** @brief 值/错误二态返回：成功取 `TRet`，失败取 `Errcode` */
template <typename TRet> using ErrRlt = bbt::core::util::Result<TRet, Errcode>;

} // namespace bbt::core::errcode
