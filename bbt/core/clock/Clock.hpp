#pragma once
#include <chrono>
#include <iostream>
#include <bbt/core/Define.hpp>
#include <cstring>

namespace bbt::core::clock
{

/* 时间值常量（Issue #12：原 bbt::core::clock::def 子命名空间压平至本层；
 * 其中 HourOfDay/DayOfWeek 与同名函数冲突，更名为 HoursPerDay/DaysPerWeek） */
BBT_TIME_CONVERT_TYPE MinOfHour = 60;
BBT_TIME_CONVERT_TYPE HoursPerDay = 24;
BBT_TIME_CONVERT_TYPE DaysPerWeek = 7;
BBT_TIME_CONVERT_TYPE SecOfMin = 60;
BBT_TIME_CONVERT_TYPE SecOfHour = SecOfMin * MinOfHour;
BBT_TIME_CONVERT_TYPE SecOfDay = SecOfMin * MinOfHour * HoursPerDay;
BBT_TIME_CONVERT_TYPE SecOfWeek = SecOfDay * DaysPerWeek;


using namespace std::chrono;
using namespace std::chrono_literals;
typedef std::chrono::nanoseconds ns;
typedef std::chrono::microseconds us;
typedef std::chrono::milliseconds ms;
typedef std::chrono::seconds s;
typedef std::chrono::minutes min;
typedef std::chrono::hours hours;

template<typename T = ms>
using Timestamp = std::chrono::time_point<std::chrono::system_clock,T>;

template<typename T = ms>
using TimestampMono = std::chrono::time_point<std::chrono::steady_clock, T>;

template<typename T = ms>
using Duration = std::chrono::duration<ms>;


/**
 * @brief 获取当前时间戳
 * @return Timestamp 时间戳
 */
template<class timeaccuracy = ms>
inline Timestamp<timeaccuracy> utcnow()
{
    return std::chrono::time_point_cast<timeaccuracy>(std::chrono::system_clock::now());
}

template<class timeaccuracy = ms>
inline TimestampMono<timeaccuracy> utcnow_mono()
{
    return std::chrono::time_point_cast<timeaccuracy>(std::chrono::steady_clock::now());
}

template<class timeaccuracy = ms>
inline Timestamp<timeaccuracy> now()
{
    return std::chrono::time_point_cast<timeaccuracy>(std::chrono::system_clock::now());
}


template<class timeaccuracy = ms>
inline TimestampMono<timeaccuracy> now_mono()
{
    return std::chrono::time_point_cast<timeaccuracy>(std::chrono::steady_clock::now());
}

/**
 * @brief 当前时间加interval后的时间戳
 *
 * @param interval 加上多久时间（单位ns）
 * @return Timestamp 添加之后的时间戳
 */
template<class timeaccuracy = ms, class Tsp = Timestamp<timeaccuracy>>
inline Tsp nowAfter(timeaccuracy interval)
{ return now<timeaccuracy>() + interval; }

/**
 * @brief 当前时间减去interval后的时间戳
 *
 * @param interval 减去的多久时间（单位ns）
 * @return Timestamp 减去之后的时间戳
 */
template<class timeaccuracy = ms, class Tsp = Timestamp<timeaccuracy>>
inline Tsp nowBefore(timeaccuracy interval)
{ return now<timeaccuracy>() - interval; }

template<class timeaccuracy = ms>
inline uint64_t gettime()
{ return now<timeaccuracy>().time_since_epoch().count(); }

template<class timeaccuracy = ms>
inline uint64_t gettime_mono()
{ return now_mono<timeaccuracy>().time_since_epoch().count(); }

/**
 * @brief 从1970年1月1日0时，到ts的ms数
 *
 * @param ts 某个时间点
 * @return time_t 从UTC 到 ts 的毫秒数
 */
inline time_t utcms(Timestamp<ns>&& ts)
{
    return ts.time_since_epoch().count()/1000/1000;
}


/**
 * @brief 获取ts所在月的日期
 *
 * @param ts 某时间点
 * @return time_t 日期 （0-30）
 */
inline time_t day(Timestamp<ns>&& ts= now<ns>())
{
    time_t tt = system_clock::to_time_t(ts);
    tm utc_tm = *gmtime(&tt);
    return utc_tm.tm_mday;
}


/**
 * @brief 获取ts所在年份的月份
 *
 * @param ts 某时间点
 * @return time_t 月份 （0-11）
 */
inline time_t month(Timestamp<ns>&& ts=now<ns>())
{
    time_t s = utcms(std::move(ts))/1000;    //ms
    return (s%(31556952)/(2629746));
}


/**
 * @brief 获取ts所在日期的几点钟
 *
 * @param ts 某时间点
 * @return time_t (0-23)
 */
inline time_t hour(Timestamp<ns>&& ts=now<ns>())
{
    time_t s = utcms(std::move(ts))/1000;    //ms
    return (s%(86400))/(3600);
}

/**
 * @brief 获取ts的年份
 *
 * @param ts 某时间点
 * @return time_t 当前年份
 */
inline time_t year(Timestamp<ns>&& ts=now<ns>())
{
    time_t tt = system_clock::to_time_t(ts);
    tm utc_tm = *gmtime(&tt);
    return 1900+utc_tm.tm_year;

}

/**
 * @brief 获取所在的分钟
 *
 * @param ts
 * @return time_t
 */
inline time_t minute(Timestamp<ns>&& ts=now<ns>())
{
    time_t tt = system_clock::to_time_t(ts);
    tm utc_tm = *gmtime(&tt);
    return utc_tm.tm_min;
}

/**
 * @brief 获取所在的秒
 *
 * @param ts
 * @return time_t
 */
inline time_t second(Timestamp<ns>&& ts=now<ns>())
{
    time_t tt = system_clock::to_time_t(ts);
    tm utc_tm = *gmtime(&tt);
    return utc_tm.tm_sec;
}

/**
 * @brief 获取所在的毫秒
 *
 * @param ts
 * @return time_t
 */
inline time_t millisecond(Timestamp<ns>&& ts=now<ns>())
{
    time_t ms = utcms(std::move(ts));
    return (ms%1000);
}

inline std::string getnow_str()
{
    std::string str{""};
    str.resize(256);
    auto now = clock::now();
	//通过不同精度获取相差的毫秒数
	uint64_t dis_millseconds = ms(now.time_since_epoch()).count() % 1000;
	time_t tt = std::chrono::system_clock::to_time_t(now);
	tm* tm_time = localtime(&tt);

    snprintf(str.data(), str.size(), "[%4d-%02d-%02d %02d:%02d:%02d %04d]",
                    tm_time->tm_year + 1900, tm_time->tm_mon + 1, tm_time->tm_mday,
                    tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec, static_cast<int>(dis_millseconds));

    str.resize(strlen(str.data()));

    return str;
}

inline std::string tostr(Timestamp<> ts)
{
    std::string str{""};
    str.resize(256);
    //通过不同精度获取相差的毫秒数
    uint64_t dis_millseconds = ms(ts.time_since_epoch()).count() % 1000;
    time_t tt = std::chrono::system_clock::to_time_t(ts);
    tm* tm_time = localtime(&tt);

    snprintf(str.data(), str.size(), "[%4d-%02d-%02d %02d:%02d:%02d %04d]",
                    tm_time->tm_year + 1900, tm_time->tm_mon + 1, tm_time->tm_mday,
                    tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec, static_cast<int>(dis_millseconds));

    str.resize(strlen(str.data()));

    return str;
}


/**
 * @brief ts 是否小于 now
 *
 * @param ts
 * @return true ts超时了,false ts未超时
 */
template<class type,class Tsp = Timestamp<type>>
inline bool is_expired(Tsp ts)
{
    if( now<type>() >= ts )
        return true;
    else
        return false;
}


/*****************************************************
 *  特殊时期函数,大多数精度到秒级
 *****************************************************/

/**
 * @brief 获取1970.1.1 00:00:00 到当前时间的秒数
 * @return uint32_t
 */
static inline uint32_t UTCTime()
{ return ::time(NULL); }

/**
 * @brief 获取有时区偏移的当前秒数(虚拟时间,一般用来做UTC时间计算)
 * @return uint32_t
 */
static inline uint32_t ZoneTime()
{
    time_t utc_now = ::time(NULL);
    time_t gmt_now = ::mktime(gmtime(&utc_now));
    bool isdst = (*localtime(&gmt_now)).tm_isdst;
    int offset_sec = ((( utc_now - gmt_now )) + ( isdst ? SecOfHour : 0 ));
    return (utc_now + offset_sec);
}

/**
 * @brief 获取本周第几天
 * @param time 毫秒时间戳
 * @return int 本周第几天(1-7) (ps: 周一 00:00:00,其实还是周日,如果想要实现提前1s,入参提前1s即可)
 */
static inline int DayOfWeek(time_t secs)
{
    secs %= SecOfWeek;
    int day = secs/SecOfDay;
    return ((day+3)%7+1);
}

/**
 * @brief 获取当天的第几个小时
 * @param millisecond 时间戳
 * @return int 当天第几个小时(1-24) (ps: 某小时 00:00,其实还是当前小时,如果想要实现提前1s,入参提前1s即可)
 */
static inline int HourOfDay(time_t secs)
{
    secs %= SecOfDay;
    int nhours = secs/SecOfHour;
    return nhours+1;
}

/**
 * @brief ts 是否小于 now
 *
 * @param ts
 * @return true ts超时了,false ts未超时
 */
template<class type,class Tsp = Timestamp<type>>
inline bool expired(Tsp ts)
{
    if( now<type>() >= ts )
        return true;
    else
        return false;
}

} // namespace bbt::core::clock
