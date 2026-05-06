#pragma once

#include <deque>
#include <string>

#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

class Coroutine;

class Trace
{
public:
    typedef std::unique_ptr<Trace> UPtr;

    static UPtr&                    GetInstance();

    void                            ResetFilters();
    void                            SetFilter(const CoroutineTraceFilter& filter);

    void                            OnCoroutineCreate(Coroutine* coroutine);
    void                            OnCoroutineEnqueue(Coroutine* coroutine, ProcesserId processer_id, int reason);
    void                            OnCoroutineResume(Coroutine* coroutine, ProcesserId processer_id);
    void                            OnCoroutineYield(Coroutine* coroutine, ProcesserId processer_id, int reason);
    void                            OnCoroutineFinish(Coroutine* coroutine);
    void                            OnCoroutineTeardown(Coroutine* coroutine);
    void                            OnCoroutineMigrate(Coroutine* coroutine, ProcesserId from_processer_id, ProcesserId to_processer_id);
    void                            OnEventRegister(CoroutineId id, CoPollEventId event_id, int event, int custom_key);
    void                            OnEventTrigger(CoroutineId id, CoPollEventId event_id, int event, int custom_key);

    CoroutineTraceSnapshot          QueryCoroutine(CoroutineId id);
    std::vector<CoroutineTraceMeta> QueryActiveCoroutinesByDesc(const std::string& desc, bool prefix);
    std::string                     DumpCoroutine(CoroutineId id);
private:
    struct TraceRecord
    {
        CoroutineTraceMeta          meta;
        bool                        active{false};
        std::deque<CoroutineTraceEvent> recent_events;
    };

    Trace() = default;

    bool                            _FilterMatched(CoroutineId id, const std::string& desc) const;
    void                            _SyncMeta(TraceRecord& record, Coroutine* coroutine, bool active);
    void                            _PushEvent(TraceRecord& record, TraceEventKind kind, Coroutine* coroutine, ProcesserId processer_id, CoPollEventId event_id, int detail_value);
    void                            _PushEventById(TraceRecord& record, TraceEventKind kind, CoroutineId id, CoPollEventId event_id, int detail_value);
    std::string                     _FormatEvent(const CoroutineTraceEvent& event) const;

private:
    std::mutex                                      m_mutex;
    CoroutineTraceFilter                            m_filter;
    std::unordered_map<CoroutineId, TraceRecord>    m_active_records;
    std::unordered_map<CoroutineId, TraceRecord>    m_history_records;
};

} // namespace bbt::coroutine::detail
