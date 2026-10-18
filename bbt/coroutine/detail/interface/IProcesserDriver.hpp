#pragma once
#include <chrono>
#include <exception>
#include <memory>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

/**
 * @brief Processer driver 通用 adapter（B1，契约见
 *  agent-docs/2026-09-07-core-runtime-contract.md §11）。
 *
 * driver 由上层（如 infra reactor owner）实现，在绑定 Processer 的
 * worker 线程、非业务协程上下文被 Tick/Wait 推进；Wake 允许任意
 * 线程调用。接口不暴露任何第三方类型。
 */

enum class ProcesserDriverPhase
{
    BeforeSchedule, // 每轮调度前
    AfterSchedule,  // 每轮调度后（公平轮次 continue 同样经过）
};

enum class ProcesserDriverResult
{
    Ok,
    Failed,
};

enum class ProcesserDriverState
{
    Unbound,
    Bound,
    Failed,
    Detaching,
};

enum class ProcesserDriverStatus
{
    Ok,
    NotInitialized, // runtime 未初始化
    InvalidTarget,  // 目标不是已登记 worker
    InvalidDriver,  // 空 driver / 无法建立绑定
    Occupied,       // 已有 Bound/Failed/Detaching 归属
    StaleToken,     // 代次不匹配（旧/伪造 token）
    SelfDetach,     // 目标 worker 线程、受管业务协程或持该代 lease 的线程
};

enum class ProcesserDriverFailure
{
    None,
    Tick,
    Wait,
};

struct ProcesserDriverToken
{
    ProcesserId     m_processer_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    uint64_t        m_generation{0};

    bool operator==(const ProcesserDriverToken& rhs) const noexcept
    {
        return m_processer_id == rhs.m_processer_id && m_generation == rhs.m_generation;
    }
    bool operator!=(const ProcesserDriverToken& rhs) const noexcept { return !(*this == rhs); }
};

struct ProcesserDriverSnapshot
{
    ProcesserDriverState    m_state{ProcesserDriverState::Unbound};
    ProcesserDriverToken    m_token{};
    ProcesserDriverFailure  m_failure{ProcesserDriverFailure::None};
    std::exception_ptr      m_exception{};
};

/**
 * @brief driver 回调接口。
 *
 * 时序约束（全部由 runtime 保证调用环境，由 driver 保证回调质量）：
 *  - OnTick/Wait 只在绑定 P 的 worker 线程、非业务协程上下文调用；
 *    Tick 必须短且非阻塞，不挂起、不直接 Resume；返回 Failed 或抛出
 *    异常会被 runtime 捕获并转为可观察 Failed 态。
 *  - Wait 仅在本地/global/steal 均无 runnable 后进入；driver 必须持久
 *    记忆早到的 Wake，使「检查后、实际阻塞前」到达的 Wake 仍能打断。
 *  - Wake 为 noexcept、线程安全、有界、不等待 worker；允许外线程、
 *    并发及嵌套调用。
 *  - 所有回调均在整条调用链的 runtime 锁之外执行；driver 不得持有
 *    Processer 强引用。
 */
class IProcesserDriver
{
public:
    virtual ~IProcesserDriver() = default;

    virtual ProcesserDriverResult   OnTick(ProcesserDriverPhase phase) = 0;
    virtual ProcesserDriverResult   Wait(std::chrono::microseconds timeout) = 0;
    virtual void                    Wake() noexcept = 0;
};

}
