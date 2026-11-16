/**
 * @file IErrcode.hpp
 * @brief 错误对象的最小接口：一段可读文本 + 一个整型错误类型。
 */
#pragma once
#include <string>
#include <functional>
#include <optional>
#include <tuple>

namespace bbt::core::errcode {

/**
 * @brief 错误类型；库内只用到 `ERR_UNKNOWN` 和 `ERR_INVALID_ARGUMENT`，使用方可自行扩展取值
 */
typedef int ErrType;

/**
 * @brief 错误类型取值
 */
enum ErrTypeEnum : ErrType {
    /**
     * 未知错误或未分类错误
     */
    ERR_UNKNOWN = -1,

    /**
     * 无效参数
     */
    ERR_INVALID_ARGUMENT = 1,

    /**
     * 内部错误；库内没有使用点，作为保留上界
     */
    ERR_MAX = 100000,
};

class IErrcode;

/**
 * @brief 错误回调签名：参数是错误对象的引用，寿命不超出回调调用期间；库内当前没有调用点
 */
typedef std::function<void(const IErrcode&)> OnErrorCallback;

/**
 * @brief 错误对象接口；`What()` 返回成员引用，`CWhat()` 的指针在文本修改后可能失效
 */
class IErrcode {
public:
    /**
     * @brief 允许经接口销毁实现对象。
     */
    virtual ~IErrcode() = default;

    /**
     * @brief 错误文本
     */
    virtual const std::string& What() const = 0;

    /**
     * @brief 错误文本的 C 字符串形式；改文本或析构后可能失效，不要缓存
     */
    virtual const char* CWhat() const = 0;

    /**
     * @brief 错误类型
     */
    virtual ErrType Type() const = 0;

private:
};

}
