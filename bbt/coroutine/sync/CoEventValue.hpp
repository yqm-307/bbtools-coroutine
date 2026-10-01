#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace bbt::coroutine
{

/**
 * @brief 唤醒载荷的类型标识：显式登记常量，不依赖 RTTI 或模板静态地址。
 *
 * 同类型在不同 DSO 中用同一个登记 ID，因此跨产物可识别；类型不符时
 * Get 直接失败，不做按尺寸的粗粒度放行。区间约定：
 *  - (kNone, kCustomBegin)      库内建基础类型
 *  - [kCustomBegin, kPointerBegin) 接口拥有者登记的自定义 POD 类型
 *  - [kPointerBegin, ...)       T* 指针（kPointerBegin + 被指类型的 ID）
 */
using CoEventValueTypeId = std::uint32_t;

inline constexpr CoEventValueTypeId kCoEventValueNone          = 0;
inline constexpr CoEventValueTypeId kCoEventValueBool          = 1;
/* char / signed char / unsigned char 是三种不同基础类型，各有独立 ID：
 * 不能因 sizeof 相同或 char 的符号性而互相识别（LP64 上 char 有符号，
 * 但 signed char 仍是不同类型）。 */
inline constexpr CoEventValueTypeId kCoEventValueChar          = 2;
inline constexpr CoEventValueTypeId kCoEventValueSChar         = 13;
inline constexpr CoEventValueTypeId kCoEventValueUChar         = 3;
inline constexpr CoEventValueTypeId kCoEventValueShort         = 4;
inline constexpr CoEventValueTypeId kCoEventValueUShort        = 5;
inline constexpr CoEventValueTypeId kCoEventValueInt           = 6;
inline constexpr CoEventValueTypeId kCoEventValueUInt          = 7;
/* long / long long 是不同基础类型（LP64 上 long 恰为 64 位，但不等于
 * long long）：各自独立 ID，不得共享同一「64 位」登记位。int64_t/uint64_t
 * 是别名，其 ID 在 traits 之后按真实 64 位基础类型派生（见下方常量）。 */
inline constexpr CoEventValueTypeId kCoEventValueLong          = 8;
inline constexpr CoEventValueTypeId kCoEventValueULong         = 9;
inline constexpr CoEventValueTypeId kCoEventValueLongLong      = 14;
inline constexpr CoEventValueTypeId kCoEventValueULongLong     = 15;
inline constexpr CoEventValueTypeId kCoEventValueFloat         = 10;
inline constexpr CoEventValueTypeId kCoEventValueDouble        = 11;
/* void 作为**被指类型**的登记 ID。指针本身的 ID 一律由下方 T* 通用公式派生
 * （含 void* 与 cv void*），本常量只标识 void 这个被指类型。 */
inline constexpr CoEventValueTypeId kCoEventValueVoid          = 12;

/** 自定义 POD 类型登记起点 */
inline constexpr CoEventValueTypeId kCoEventValueCustomBegin   = 0x100;
/** 指针类型登记起点；非指针登记 ID 必须严格小于此值 */
inline constexpr CoEventValueTypeId kCoEventValuePointerBegin  = 0x10000;

/**
 * @brief 类型标识 traits：主模板故意不可用，未登记类型在编译期拒绝。
 *
 * 常用整数/浮点/void* 已由库登记；自定义 POD 由接口拥有者在公共头
 * 用 BBT_COEVENT_VALUE_REGISTER_TYPE 登记唯一 ID。
 */
template<class T>
struct CoEventValueTraits
{
    static_assert(!std::is_same<T, T>::value,
        "CoEventValue: 类型未登记，请用 BBT_COEVENT_VALUE_REGISTER_TYPE 登记唯一 ID");
};

template<> struct CoEventValueTraits<bool>              { static constexpr CoEventValueTypeId id = kCoEventValueBool; };
template<> struct CoEventValueTraits<char>              { static constexpr CoEventValueTypeId id = kCoEventValueChar; };
template<> struct CoEventValueTraits<signed char>       { static constexpr CoEventValueTypeId id = kCoEventValueSChar; };
template<> struct CoEventValueTraits<unsigned char>     { static constexpr CoEventValueTypeId id = kCoEventValueUChar; };
template<> struct CoEventValueTraits<short>             { static constexpr CoEventValueTypeId id = kCoEventValueShort; };
template<> struct CoEventValueTraits<unsigned short>    { static constexpr CoEventValueTypeId id = kCoEventValueUShort; };
template<> struct CoEventValueTraits<int>               { static constexpr CoEventValueTypeId id = kCoEventValueInt; };
template<> struct CoEventValueTraits<unsigned int>      { static constexpr CoEventValueTypeId id = kCoEventValueUInt; };
template<> struct CoEventValueTraits<long>              { static constexpr CoEventValueTypeId id = kCoEventValueLong; };
template<> struct CoEventValueTraits<unsigned long>     { static constexpr CoEventValueTypeId id = kCoEventValueULong; };
template<> struct CoEventValueTraits<long long>         { static constexpr CoEventValueTypeId id = kCoEventValueLongLong; };
template<> struct CoEventValueTraits<unsigned long long>{ static constexpr CoEventValueTypeId id = kCoEventValueULongLong; };
/* int64_t / uint64_t 不单独特化：它们是 long 或 long long 的别名，别名重复
 * 特化是编译错误；其 ID 由下方常量按真实基础类型派生，保证跨平台自洽。 */
template<> struct CoEventValueTraits<float>             { static constexpr CoEventValueTypeId id = kCoEventValueFloat; };
template<> struct CoEventValueTraits<double>            { static constexpr CoEventValueTypeId id = kCoEventValueDouble; };
/* void 只登记为「被指类型」，void* / cv void* 全部走下方 T* 通用公式。
 *
 * 历史实现给 void* 与 cv void* 写了独立全特化，数值恰好与通用公式
 * 「被指 cv + 一层指针」的派生结果重合（const void* 与 void* const* 同为
 * 65585，volatile / cv 两对同理，共 3 组），于是错型 Get 能按 sizeof 读回
 * 8 字节并改写 out。根因单一：void 与 void* 共用了同一个登记 ID（12），
 * 被指类型与指针类型混为一谈。此处分离二者即可，不需要第二套编号。
 */
template<> struct CoEventValueTraits<void>              { static constexpr CoEventValueTypeId id = kCoEventValueVoid; };

/**
 * @brief 指针层级：T* 的层级 = 被指类型的层级 + 1（非指针类型为 0）。
 *
 * 仅用于下方 T* 派生时的容量检查，不参与编号本身。
 */
template<class T>
struct CoEventValuePointerDepth
{
    static constexpr unsigned value = 0u;
};

template<class T>
struct CoEventValuePointerDepth<T*>
{
    static constexpr unsigned value = 1u + CoEventValuePointerDepth<T>::value;
};

/**
 * @brief 指针类型可编码的最大层级（含被指 cv 的 2 bit 在内每层乘 4）。
 *
 * 容量推导：id(T*) = kPointerBegin + id(被指) * 4 + cv。被指 ID 最大为
 * kPointerBegin - 1 = 65535（自定义登记区间上界），故最坏情况第 7 层
 * 上界 1431633919 < 2^32（uint32 无回绕）；第 8 层最坏值超过 2^32，
 * 回绕后会与低层 ID 完全重合（历史缺陷：16/17 层 int 指针曾给出同一个
 * ID），类型身份不再单射。因此超过本上界的指针类型在编译期显式拒绝，
 * 不做静默回绕、也不用 sizeof 兜底。
 */
inline constexpr unsigned kCoEventValueMaxPointerDepth = 7u;

/**
 * @brief 指针类型不塌缩成单一 Pointer：ID 由被指类型的登记 ID 与被指 cv
 * 限定共同派生，因此
 *  - 不同被指类型互不识别；
 *  - `const T*` 不得当作 `T*` 读回（被指 cv 是身份的一部分）；
 *  - 多级指针（T** / T***）逐级派生，层级不同即层级不同的 ID；
 *  - 同一类型跨 DSO 仍一致（全部来自公共头的显式常量，不用 RTTI/地址）；
 *  - 层级受 kCoEventValueMaxPointerDepth 约束，更深指针编译期报错。
 */
template<class T>
struct CoEventValueTraits<T*>
{
    static_assert(CoEventValuePointerDepth<T>::value + 1u <= kCoEventValueMaxPointerDepth,
        "CoEventValue: 指针层级超过 kCoEventValueMaxPointerDepth，ID 会回绕并与低位类型碰撞");
    static constexpr CoEventValueTypeId id =
        kCoEventValuePointerBegin +
        CoEventValueTraits<std::remove_cv_t<T>>::id * 4u +
        (std::is_const<T>::value ? 1u : 0u) +
        (std::is_volatile<T>::value ? 2u : 0u);
};

/**
 * @brief 平台 64 位整数别名（std::int64_t / std::uint64_t）的登记 ID。
 *
 * 别名不做独立特化，而是取真实基础类型的 ID：LP64 下 int64_t 即 long
 * （kCoEventValueLong），long long 另有独立 ID，两者互不识别；LLP64 下
 * int64_t 即 long long，语义同样自洽。
 */
inline constexpr CoEventValueTypeId kCoEventValueInt64  = CoEventValueTraits<std::int64_t>::id;
inline constexpr CoEventValueTypeId kCoEventValueUInt64 = CoEventValueTraits<std::uint64_t>::id;

/**
 * 登记自定义 POD：ID 必须落在自定义区间，且各 DSO 使用同一常量。
 *
 * 登记必须写在**全局命名空间**（或 `bbt::coroutine` / `bbt`）作用域：显式特化
 * 只能声明在包裹模板命名空间的命名空间里（与 `std::hash` 特化同一条语言规则），
 * 放进用户命名空间或匿名命名空间会报 “does not enclose” 错误。
 * 成员与断言里的类型名用全限定 ::bbt::coroutine::... 引用，避免被同名类型遮蔽。
 */
#define BBT_COEVENT_VALUE_REGISTER_TYPE(TYPE, ID)                                        \
    template<> struct bbt::coroutine::CoEventValueTraits<TYPE>                            \
    {                                                                                     \
        static constexpr ::bbt::coroutine::CoEventValueTypeId id = (ID);                  \
        static_assert(id >= ::bbt::coroutine::kCoEventValueCustomBegin &&                 \
                      id <  ::bbt::coroutine::kCoEventValuePointerBegin,                  \
                      "CoEventValue: 自定义类型 ID 必须落在自定义登记区间");               \
    }

/**
 * @brief 协程唤醒的内联小载荷（带参 Notify / 带参等待）。
 *
 * 语义要点：
 * - 固定 kInlineSize(24) 字节内联缓冲，不分配；超过容量或非平凡拷贝
 *   类型在编译期拒绝。
 * - 按字节 memcpy 存取，不从缓冲强转成 T* 读取，避免对齐/别名假设。
 * - 类型标识走显式登记 ID（CoEventValueTraits），类型不符时 Get 返回
 *   false 且不修改 out；空值同样返回 false。
 * - 指针只借用不延寿：对象须至少存活到等待者恢复并取用完毕。
 */
class CoEventValue
{
public:
    static constexpr std::size_t kInlineSize = 24;

    CoEventValue() noexcept = default;

    bool HasValue() const noexcept { return m_type != kCoEventValueNone; }

    template<class T>
    static CoEventValue From(const T& value) noexcept
    {
        using Type = std::remove_cv_t<T>;
        static_assert(std::is_trivially_copyable<Type>::value,
            "CoEventValue: 仅支持可平凡拷贝类型");
        static_assert(sizeof(Type) <= kInlineSize,
            "CoEventValue: 载荷超过 24 字节内联容量");

        CoEventValue out;
        out.m_type = CoEventValueTraits<Type>::id;
        out.m_size = static_cast<std::uint8_t>(sizeof(Type));
        std::memcpy(out.m_buf, &value, sizeof(Type));
        return out;
    }

    template<class T>
    bool Get(T& out) const noexcept
    {
        using Type = std::remove_cv_t<T>;
        static_assert(std::is_trivially_copyable<Type>::value,
            "CoEventValue: 仅支持可平凡拷贝类型");
        static_assert(sizeof(Type) <= kInlineSize,
            "CoEventValue: 载荷超过 24 字节内联容量");

        if (m_type == kCoEventValueNone ||
            m_type != CoEventValueTraits<Type>::id ||
            m_size != sizeof(Type))
            return false;

        std::memcpy(&out, m_buf, sizeof(Type));
        return true;
    }

private:
    alignas(std::max_align_t) unsigned char m_buf[kInlineSize]{};
    std::uint8_t            m_size{0};
    CoEventValueTypeId      m_type{kCoEventValueNone};
};

} // namespace bbt::coroutine
