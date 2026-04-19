#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <ostream>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

enum class WaitResult : int
{
    WAIT_ERROR = -1,
    WAIT_READY = 0,
    WAIT_TIMEOUT = 1,
    WAIT_CLOSED = 2,
    WAIT_CANCELED = 3,
    WAIT_NONE = 4,
};

enum class WaitState : int
{
    WAIT_INIT = 0,
    WAIT_PENDING = 1,
    WAIT_COMPLETED = 2,
    WAIT_TIMED_OUT = 3,
    WAIT_CLOSED = 4,
    WAIT_CANCELED = 5,
    WAIT_FAILED = 6,
};

inline std::ostream& operator<<(std::ostream& os, WaitResult result)
{
    return os << static_cast<int>(result);
}

inline std::ostream& operator<<(std::ostream& os, WaitState state)
{
    return os << static_cast<int>(state);
}

class WaitNode final
{
public:
    using Id = uint64_t;

    WaitNode():
        m_id(_GenerateId())
    {
    }

    Id GetId() const noexcept
    {
        return m_id;
    }

    bool Arm() noexcept
    {
        auto expected = WaitState::WAIT_INIT;
        return m_state.compare_exchange_strong(expected, WaitState::WAIT_PENDING);
    }

    bool Complete(WaitResult result) noexcept
    {
        if (!_IsTerminalResult(result))
            return false;

        auto expected = WaitState::WAIT_PENDING;
        auto target = _StateFromResult(result);
        if (!m_state.compare_exchange_strong(expected, target))
            return false;

        m_result.store(result);
        return true;
    }

    bool CompleteReady() noexcept
    {
        return Complete(WaitResult::WAIT_READY);
    }

    bool CompleteTimeout() noexcept
    {
        return Complete(WaitResult::WAIT_TIMEOUT);
    }

    bool CompleteClosed() noexcept
    {
        return Complete(WaitResult::WAIT_CLOSED);
    }

    bool CompleteCanceled() noexcept
    {
        return Complete(WaitResult::WAIT_CANCELED);
    }

    bool CompleteError() noexcept
    {
        return Complete(WaitResult::WAIT_ERROR);
    }

    WaitState GetState() const noexcept
    {
        return m_state.load();
    }

    WaitResult GetResult() const noexcept
    {
        return m_result.load();
    }

    bool IsPending() const noexcept
    {
        return GetState() == WaitState::WAIT_PENDING;
    }

    bool IsTerminal() const noexcept
    {
        return !_IsPendingState(GetState()) && GetState() != WaitState::WAIT_INIT;
    }

private:
    static Id _GenerateId() noexcept
    {
        static std::atomic_uint64_t next_id{0};
        return ++next_id;
    }

    static bool _IsPendingState(WaitState state) noexcept
    {
        return state == WaitState::WAIT_PENDING;
    }

    static bool _IsTerminalResult(WaitResult result) noexcept
    {
        return result != WaitResult::WAIT_NONE;
    }

    static WaitState _StateFromResult(WaitResult result) noexcept
    {
        switch (result)
        {
        case WaitResult::WAIT_READY:
            return WaitState::WAIT_COMPLETED;
        case WaitResult::WAIT_TIMEOUT:
            return WaitState::WAIT_TIMED_OUT;
        case WaitResult::WAIT_CLOSED:
            return WaitState::WAIT_CLOSED;
        case WaitResult::WAIT_CANCELED:
            return WaitState::WAIT_CANCELED;
        case WaitResult::WAIT_ERROR:
            return WaitState::WAIT_FAILED;
        default:
            return WaitState::WAIT_INIT;
        }
    }

private:
    Id m_id;
    std::atomic<WaitState> m_state{WaitState::WAIT_INIT};
    std::atomic<WaitResult> m_result{WaitResult::WAIT_NONE};
};

class WaitProtocolBridge final
{
public:
    static std::shared_ptr<CoPollEvent> CreateCustomWait(Coroutine& coroutine, int key, int timeout_ms = -1)
    {
        if (timeout_ms >= 0)
            return coroutine.RegistCustom(key, timeout_ms);

        return coroutine.RegistCustom(key);
    }

    static WaitResult AwaitArmedEvent(Coroutine& coroutine,
                                      const std::shared_ptr<CoPollEvent>& event,
                                      const CoroutineOnYieldCallback& on_yield = nullptr)
    {
        if (event == nullptr)
            return WaitResult::WAIT_ERROR;

        int ret = coroutine.YieldWithCallback([event, on_yield]() {
            if (event->Regist() != 0)
                return false;

            if (on_yield == nullptr)
                return true;

            return on_yield();
        });

        if (ret != 0)
            return WaitResult::WAIT_ERROR;

        return ResultFromResumeEvent(coroutine.GetLastResumeEvent());
    }

    static WaitResult WaitFd(Coroutine& coroutine, int fd, short events, int timeout_ms = 0)
    {
        if (coroutine.YieldUntilFdEx(fd, events, timeout_ms) != 0)
            return WaitResult::WAIT_ERROR;

        return ResultFromResumeEvent(coroutine.GetLastResumeEvent());
    }

    static WaitResult WaitTimeout(Coroutine& coroutine, int timeout_ms)
    {
        if (coroutine.YieldUntilTimeout(timeout_ms) != 0)
            return WaitResult::WAIT_ERROR;

        return ResultFromResumeEvent(coroutine.GetLastResumeEvent());
    }

    static WaitResult ResultFromResumeEvent(int resume_event)
    {
        if (resume_event & POLL_EVENT_TIMEOUT)
            return WaitResult::WAIT_TIMEOUT;

        if (resume_event & (POLL_EVENT_CUSTOM | POLL_EVENT_READABLE | POLL_EVENT_WRITEABLE))
            return WaitResult::WAIT_READY;

        return WaitResult::WAIT_READY;
    }

    static int ToLegacyReturnCode(WaitResult result)
    {
        switch (result)
        {
        case WaitResult::WAIT_READY:
            return 0;
        case WaitResult::WAIT_TIMEOUT:
            return 1;
        default:
            return -1;
        }
    }
};

}