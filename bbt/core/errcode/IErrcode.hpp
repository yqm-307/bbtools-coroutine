#pragma once
#include <string>
#include <functional>
#include <optional>
#include <tuple>

namespace bbt::core::errcode
{

typedef int ErrType;


enum ErrTypeEnum : ErrType
{
    ERR_UNKNOWN             = -1,       // 未知错误|未分类错误
    ERR_INVALID_ARGUMENT    = 1,        // 无效参数
    ERR_MAX                 = 100000,   // 内部错误
};


class IErrcode;
typedef std::function<void(const IErrcode&)> OnErrorCallback;

class IErrcode
{
public:
    virtual ~IErrcode() = default;

    virtual const std::string&  What()  const = 0;
    virtual const char*         CWhat() const = 0;
    virtual ErrType             Type()  const = 0;
private:
};

} // namespace bbt::core::errcode