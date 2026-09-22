#pragma once

#include "IErrcode.hpp"
#include <bbt/core/util/Result.hpp>

namespace bbt::core::errcode
{

class Errcode:
    public IErrcode
{
public:
    Errcode();
    Errcode(const std::string& msg, ErrType type = ERR_UNKNOWN);
    Errcode(const Errcode& other);
    Errcode(Errcode&& other);
    virtual ~Errcode();

    Errcode& operator=(const Errcode& other);
    Errcode& operator=(Errcode&& other);

    virtual const std::string& What() const override;
    virtual const char* CWhat() const override;
    virtual ErrType Type() const override;

protected:
    ErrType             m_err_type{-1};
    std::string         m_err_msg{""};
};

typedef std::optional<bbt::core::errcode::Errcode> ErrOpt;

template<typename ...TRetTuple>
using ErrTuple = std::tuple<ErrOpt, TRetTuple...>;

template<typename TRet>
using ErrRlt = bbt::core::util::Result<TRet, Errcode>;


} // namespace bbt::core::errcode
