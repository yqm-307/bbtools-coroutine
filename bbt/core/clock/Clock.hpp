/**
 * @file Clock.hpp
 * @brief 时间换算常量、`std::chrono` 别名与取时/日历换算函数；`Timestamp` 是墙上时间会跳变，量间隔用 `*_mono`。
 */
#pragma once
#include <chrono>
#include <iostream>
#include <bbt/core/Define.hpp>
#include <cstring>

namespace bbt::core::clock {

/**
 * 时间换算常量。复数命名（HoursPerDay/DaysPerWeek）是为了不和同名函数 HourOfDay/DayOfWeek 撞名。
 */

/**
 * 一小时的分钟数
 */
BBT_TIME_CONVERT_TYPE MinOfHour = 60;

/**
 * 一天的小时数
 */
BBT_TIME_CONVERT_TYPE HoursPerDay = 24;

/**
 * 一周的天数
 */
BBT_TIME_CONVERT_TYPE DaysPerWeek = 7;

/**
 * 一分钟的秒数
 */
BBT_TIME_CONVERT_TYPE SecOfMin = 60;

/**
 * 一小时的秒数
 */
BBT_TIME_CONVERT_TYPE SecOfHour = SecOfMin * MinOfHour;

/**
 * 一天的秒数
 */
BBT_TIME_CONVERT_TYPE SecOfDay = SecOfMin * MinOfHour * HoursPerDay;

/**
 * 一周的秒数
 */
BBT_TIME_CONVERT_TYPE SecOfWeek = SecOfDay * DaysPerWeek;

using namespace std::chrono;
using namespace std::chrono_literals;
typedef std::chrono::nanoseconds ns;
typedef std::chrono::microseconds us;
typedef std::chrono::milliseconds ms;
typedef std::chrono::seconds s;
typedef std::chrono::minutes min;
typedef std::chrono::hours hours;

/**
 * @brief 墙上时间点（`system_clock`），精度由模板参数决定，默认毫秒
 */
template <typename T = ms> using Timestamp = std::chrono::time_point<std::chrono::system_clock, T>;

/**
 * @brief 单调时间点（`steady_clock`），用于量间隔；不受改系统时间影响，但不对应日历
 */
template <typename T = ms> using TimestampMono = std::chrono::time_point<std::chrono::steady_clock, T>;

/**
 * @brief 时长别名；当前固定为毫秒，模板参数不参与定义
 */
template <typename T = ms> using Duration = std::chrono::duration<ms>;

/**
 * @brief 获取当前墙上时间点（`system_clock`），按 `timeaccuracy` 截断精度
 * @return Timestamp<timeaccuracy> 当前时间点
 */
template <class timeaccuracy = ms> inline Timestamp<timeaccuracy> utcnow()
{
    return std::chrono::time_point_cast<timeaccuracy>(std::chrono::system_clock::now());
}

/**
 * @brief 当前单调时间点（`steady_clock`），按 `timeaccuracy` 截断；只用于量间隔，epoch 无日历含义
 */
template <class timeaccuracy = ms> inline TimestampMono<timeaccuracy> utcnow_mono()
{
    return std::chrono::time_point_cast<timeaccuracy>(std::chrono::steady_clock::now());
}

/**
 * @brief 与 `utcnow` 同义，取当前墙上时间点
 */
template <class timeaccuracy = ms> inline Timestamp<timeaccuracy> now()
{
    return std::chrono::time_point_cast<timeaccuracy>(std::chrono::system_clock::now());
}

/**
 * @brief 与 `utcnow_mono` 同义，取当前单调时间点
 */
template <class timeaccuracy = ms> inline TimestampMono<timeaccuracy> now_mono()
{
    return std::chrono::time_point_cast<timeaccuracy>(std::chrono::steady_clock::now());
}

/**
 * @brief 当前时间加上 interval 后的时间点
 * @param interval 单位由 `timeaccuracy` 决定（默认毫秒），不是固定 ns
 */
template <class timeaccuracy = ms, class Tsp = Timestamp<timeaccuracy>> inline Tsp nowAfter(timeaccuracy interval)
{
    return now<timeaccuracy>() + interval;
}

/**
 * @brief 当前时间减去 interval 后的时间点
 * @param interval 单位由 `timeaccuracy` 决定（默认毫秒），不是固定 ns
 */
template <class timeaccuracy = ms, class Tsp = Timestamp<timeaccuracy>> inline Tsp nowBefore(timeaccuracy interval)
{
    return now<timeaccuracy>() - interval;
}

/**
 * @brief 取当前墙上时间的计数值（默认毫秒）；两次相减跨校时会失真，量间隔用 `gettime_mono`
 */
template <class timeaccuracy = ms> inline uint64_t gettime()
{
    return now<timeaccuracy>().time_since_epoch().count();
}

/**
 * @brief 取当前单调时间的计数值（默认毫秒）；适合算耗时、判超时，不要当时间戳格式化
 */
template <class timeaccuracy = ms> inline uint64_t gettime_mono()
{
    return now_mono<timeaccuracy>().time_since_epoch().count();
}

/**
 * @brief 把 ns 精度的墙上时间点换成自 1970-01-01 UTC 起的毫秒数，不足 1ms 截断
 * @param ts 右值引用，只能绑临时对象或 std::move 后的值
 */
inline time_t utcms(Timestamp<ns>&& ts)
{
    return ts.time_since_epoch().count() / 1000 / 1000;
}

/**
 * @brief 获取 ts 所在月份的第几天（UTC），1-31
 * @param ts ns 精度，缺省取当前时间
 */
inline time_t day(Timestamp<ns>&& ts = now<ns>())
{
    time_t tt = system_clock::to_time_t(ts);
    tm utc_tm = *gmtime(&tt);
    return utc_tm.tm_mday;
}

/**
 * @brief 获取 ts 所在年份的月份（UTC），0-11；按平均年长/月长整除换算，闰年与长短月附近会差一个月
 * @param ts ns 精度，缺省取当前时间
 */
inline time_t month(Timestamp<ns>&& ts = now<ns>())
{
    /**
     * 换算到秒：utcms 返回毫秒，除以 1000 后按秒参与日历取模
     */
    time_t s = utcms(std::move(ts)) / 1000;
    return (s % (31556952) / (2629746));
}

/**
 * @brief 获取 ts 所在日期的几点钟（UTC），0-23
 * @param ts ns 精度，缺省取当前时间
 */
inline time_t hour(Timestamp<ns>&& ts = now<ns>())
{
    /**
     * 换算到秒：utcms 返回毫秒，除以 1000 后按秒参与当天取模
     */
    time_t s = utcms(std::move(ts)) / 1000;
    return (s % (86400)) / (3600);
}

/**
 * @brief 获取 ts 的年份（UTC），如 2026
 * @param ts ns 精度，缺省取当前时间
 */
inline time_t year(Timestamp<ns>&& ts = now<ns>())
{
    time_t tt = system_clock::to_time_t(ts);
    tm utc_tm = *gmtime(&tt);
    return 1900 + utc_tm.tm_year;
}

/**
 * @brief 获取 ts 所在分钟的分钟数（UTC），0-59
 * @param ts ns 精度，缺省取当前时间
 */
inline time_t minute(Timestamp<ns>&& ts = now<ns>())
{
    time_t tt = system_clock::to_time_t(ts);
    tm utc_tm = *gmtime(&tt);
    return utc_tm.tm_min;
}

/**
 * @brief 获取 ts 所在秒的秒数（UTC），0-59
 * @param ts ns 精度，缺省取当前时间
 */
inline time_t second(Timestamp<ns>&& ts = now<ns>())
{
    time_t tt = system_clock::to_time_t(ts);
    tm utc_tm = *gmtime(&tt);
    return utc_tm.tm_sec;
}

/**
 * @brief 获取 ts 的毫秒字段（UTC），0-999
 * @param ts ns 精度，缺省取当前时间
 */
inline time_t millisecond(Timestamp<ns>&& ts = now<ns>())
{
    time_t ms = utcms(std::move(ts));
    return (ms % 1000);
}

/**
 * @brief 把当前本地时间格式化成 `[YYYY-MM-DD HH:MM:SS mmm]`；localtime 用共享静态缓冲，多线程并发会互相覆盖
 */
inline std::string getnow_str()
{
    std::string str{""};
    str.resize(256);
    auto now = clock::now();
    /**
     * 通过不同精度获取相差的毫秒数
     */
    uint64_t dis_millseconds = ms(now.time_since_epoch()).count() % 1000;
    time_t tt = std::chrono::system_clock::to_time_t(now);
    tm* tm_time = localtime(&tt);

    snprintf(str.data(), str.size(), "[%4d-%02d-%02d %02d:%02d:%02d %04d]", tm_time->tm_year + 1900,
             tm_time->tm_mon + 1, tm_time->tm_mday, tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec,
             static_cast<int>(dis_millseconds));

    str.resize(strlen(str.data()));

    return str;
}

/**
 * @brief 把指定的墙上时间点按本地时区格式化成 `[YYYY-MM-DD HH:MM:SS mmm]`
 * @param ts 毫秒精度的墙上时间点（`Timestamp<>` 即 `Timestamp<ms>`）
 */
inline std::string tostr(Timestamp<> ts)
{
    std::string str{""};
    str.resize(256);
    /**
     * 通过不同精度获取相差的毫秒数
     */
    uint64_t dis_millseconds = ms(ts.time_since_epoch()).count() % 1000;
    time_t tt = std::chrono::system_clock::to_time_t(ts);
    tm* tm_time = localtime(&tt);

    snprintf(str.data(), str.size(), "[%4d-%02d-%02d %02d:%02d:%02d %04d]", tm_time->tm_year + 1900,
             tm_time->tm_mon + 1, tm_time->tm_mday, tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec,
             static_cast<int>(dis_millseconds));

    str.resize(strlen(str.data()));

    return str;
}

/**
 * @brief ts 是否已经到期（与当前墙上时间比较）
 * @param ts 到期时间点，精度由 `type` 指定
 */
template <class type, class Tsp = Timestamp<type>> inline bool is_expired(Tsp ts)
{
    if (now<type>() >= ts)
        return true;
    else
        return false;
}

/**
 * 秒级与日历换算函数，入参都是 time_t 秒数而不是毫秒
 */

/**
 * @brief 获取自 1970-01-01 UTC 到现在的秒数
 */
static inline uint32_t UTCTime()
{
    return ::time(NULL);
}

/**
 * @brief 获取按本地时区偏移后的秒数（本地墙上时间的秒值）
 * @note 偏移量取 `mktime(gmtime())` 与 `time()` 之差，夏令时再按 `tm_isdst` 补一小时，不读时区数据库
 */
static inline uint32_t ZoneTime()
{
    time_t utc_now = ::time(NULL);
    time_t gmt_now = ::mktime(gmtime(&utc_now));
    bool isdst = (*localtime(&gmt_now)).tm_isdst;
    int offset_sec = (((utc_now - gmt_now)) + (isdst ? SecOfHour : 0));
    return (utc_now + offset_sec);
}

/**
 * @brief 获取本周第几天，1-7（周一为 1）；入参是秒不是毫秒
 * @note 1970-01-01 是周四，所以偏移 3 再取模
 */
static inline int DayOfWeek(time_t secs)
{
    secs %= SecOfWeek;
    int day = secs / SecOfDay;
    return ((day + 3) % 7 + 1);
}

/**
 * @brief 获取当天的第几个小时，1-24；入参是秒，整点算作该小时的第 1 秒
 */
static inline int HourOfDay(time_t secs)
{
    secs %= SecOfDay;
    int nhours = secs / SecOfHour;
    return nhours + 1;
}

/**
 * @brief ts 是否已经到期，与 `is_expired` 同义
 */
template <class type, class Tsp = Timestamp<type>> inline bool expired(Tsp ts)
{
    if (now<type>() >= ts)
        return true;
    else
        return false;
}

}
