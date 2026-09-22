#include "Errcode.hpp"

namespace bbt::core::errcode
{


Errcode::Errcode()
{
}


Errcode::Errcode(const Errcode& other):
    m_err_type(other.m_err_type),
    m_err_msg(other.m_err_msg)
{
}


Errcode::Errcode(Errcode&& other):
    m_err_type(std::move(other.m_err_type)),
    m_err_msg(std::move(other.m_err_msg))
{
}


Errcode::Errcode(const std::string& msg, ErrType type):
    m_err_type(type),
    m_err_msg(msg)
{
}



Errcode::~Errcode()
{
}

Errcode& Errcode::operator=(const Errcode& other)
{
    m_err_msg = other.m_err_msg;
    m_err_type = other.m_err_type;
    return *this;
}


Errcode& Errcode::operator=(Errcode&& other)
{
    m_err_msg = std::move(other.m_err_msg);
    m_err_type = std::move(other.m_err_type);
    return *this;
}

const std::string& Errcode::What() const
{
    return m_err_msg;
}

const char* Errcode::CWhat() const
{
    return What().c_str();
}

ErrType Errcode::Type() const
{
    return m_err_type;
}



} // namespace bbt::core::errcode