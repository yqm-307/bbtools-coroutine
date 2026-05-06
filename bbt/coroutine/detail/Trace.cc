#include <algorithm>
#include <sstream>

#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Trace.hpp>

namespace bbt::coroutine::detail
{

Trace::UPtr& Trace::GetInstance()
{
    static UPtr inst{nullptr};
    if (inst == nullptr)
        inst = UPtr(new Trace());

    return inst;
}

void Trace::ResetFilters()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_filter = {};
    m_active_records.clear();
    m_history_records.clear();
}

void Trace::SetFilter(const CoroutineTraceFilter& filter)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_filter = filter;
    m_active_records.clear();
    m_history_records.clear();
}

bool Trace::_FilterMatched(CoroutineId id, const std::string& desc) const
{
#ifndef BBT_COROUTINE_TRACE
    (void)id;
    (void)desc;
    return false;
#else
    if (m_filter.ids.empty() && m_filter.descs.empty() && m_filter.desc_prefixes.empty())
        return false;

    if (std::find(m_filter.ids.begin(), m_filter.ids.end(), id) != m_filter.ids.end())
        return true;

    if (std::find(m_filter.descs.begin(), m_filter.descs.end(), desc) != m_filter.descs.end())
        return true;

    for (const auto& prefix : m_filter.desc_prefixes) {
        if (desc.rfind(prefix, 0) == 0)
            return true;
    }

    return false;
#endif
}

void Trace::_SyncMeta(TraceRecord& record, Coroutine* coroutine, bool active)
{
    if (coroutine == nullptr)
        return;

    record.meta = coroutine->BuildTraceMeta();
    record.active = active;
}

void Trace::_PushEvent(TraceRecord& record,
                       TraceEventKind kind,
                       Coroutine* coroutine,
                       ProcesserId processer_id,
                       CoPollEventId event_id,
                       int detail_value)
{
    CoroutineTraceEvent event;
    event.kind = kind;
    event.timestamp_us = bbt::core::clock::gettime_mono<>();
    event.status = coroutine != nullptr ? coroutine->GetStatus() : detail::CoroutineStatus::CO_DEFAULT;
    event.processer_id = processer_id;
    event.poll_event_id = event_id;
    event.detail_value = detail_value;
    record.recent_events.push_back(event);

    while (record.recent_events.size() > g_bbt_coroutine_config->m_cfg_trace_history_limit)
        record.recent_events.pop_front();
}

void Trace::_PushEventById(TraceRecord& record,
                           TraceEventKind kind,
                           CoroutineId id,
                           CoPollEventId event_id,
                           int detail_value)
{
    CoroutineTraceEvent event;
    event.kind = kind;
    event.timestamp_us = bbt::core::clock::gettime_mono<>();
    event.status = record.meta.status;
    event.processer_id = record.meta.last_processer_id;
    event.poll_event_id = event_id;
    event.detail_value = detail_value;
    if (record.meta.id == BBT_COROUTINE_INVALID_COROUTINE_ID)
        record.meta.id = id;
    record.recent_events.push_back(event);

    while (record.recent_events.size() > g_bbt_coroutine_config->m_cfg_trace_history_limit)
        record.recent_events.pop_front();
}

void Trace::OnCoroutineCreate(Coroutine* coroutine)
{
#ifndef BBT_COROUTINE_TRACE
    (void)coroutine;
#else
    if (coroutine == nullptr)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const bool traced = _FilterMatched(coroutine->GetId(), coroutine->GetDesc());
    coroutine->SetTraceEnabled(traced);
    if (!traced)
        return;

    auto& record = m_active_records[coroutine->GetId()];
    _SyncMeta(record, coroutine, true);
    _PushEvent(record, TraceEventKind::CREATE, coroutine, coroutine->GetLastProcesserId(), BBT_COROUTINE_INVALID_COPOLLEVENT_ID, 0);
#endif
}

void Trace::OnCoroutineEnqueue(Coroutine* coroutine, ProcesserId processer_id, int reason)
{
#ifndef BBT_COROUTINE_TRACE
    (void)coroutine;
    (void)processer_id;
    (void)reason;
#else
    if (coroutine == nullptr || !coroutine->IsTraceEnabled())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_active_records.find(coroutine->GetId());
    if (it == m_active_records.end())
        return;

    _SyncMeta(it->second, coroutine, true);
    _PushEvent(it->second, TraceEventKind::ENQUEUE, coroutine, processer_id, BBT_COROUTINE_INVALID_COPOLLEVENT_ID, reason);
#endif
}

void Trace::OnCoroutineResume(Coroutine* coroutine, ProcesserId processer_id)
{
#ifndef BBT_COROUTINE_TRACE
    (void)coroutine;
    (void)processer_id;
#else
    if (coroutine == nullptr || !coroutine->IsTraceEnabled())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_active_records.find(coroutine->GetId());
    if (it == m_active_records.end())
        return;

    _SyncMeta(it->second, coroutine, true);
    _PushEvent(it->second, TraceEventKind::RESUME, coroutine, processer_id, BBT_COROUTINE_INVALID_COPOLLEVENT_ID, coroutine->GetLastResumeReason());
#endif
}

void Trace::OnCoroutineYield(Coroutine* coroutine, ProcesserId processer_id, int reason)
{
#ifndef BBT_COROUTINE_TRACE
    (void)coroutine;
    (void)processer_id;
    (void)reason;
#else
    if (coroutine == nullptr || !coroutine->IsTraceEnabled())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_active_records.find(coroutine->GetId());
    if (it == m_active_records.end())
        return;

    _SyncMeta(it->second, coroutine, true);
    _PushEvent(it->second, TraceEventKind::YIELD, coroutine, processer_id, BBT_COROUTINE_INVALID_COPOLLEVENT_ID, reason);
#endif
}

void Trace::OnCoroutineFinish(Coroutine* coroutine)
{
#ifndef BBT_COROUTINE_TRACE
    (void)coroutine;
#else
    if (coroutine == nullptr || !coroutine->IsTraceEnabled())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_active_records.find(coroutine->GetId());
    if (it == m_active_records.end())
        return;

    _SyncMeta(it->second, coroutine, true);
    _PushEvent(it->second, TraceEventKind::FINISH, coroutine, coroutine->GetLastProcesserId(), BBT_COROUTINE_INVALID_COPOLLEVENT_ID, 0);
#endif
}

void Trace::OnCoroutineTeardown(Coroutine* coroutine)
{
#ifndef BBT_COROUTINE_TRACE
    (void)coroutine;
#else
    if (coroutine == nullptr || !coroutine->IsTraceEnabled())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_active_records.find(coroutine->GetId());
    if (it == m_active_records.end())
        return;

    _SyncMeta(it->second, coroutine, false);
    _PushEvent(it->second, TraceEventKind::TEARDOWN, coroutine, coroutine->GetLastProcesserId(), BBT_COROUTINE_INVALID_COPOLLEVENT_ID, TRACE_REASON_TEARDOWN);
    m_history_records[coroutine->GetId()] = it->second;
    m_active_records.erase(it);
#endif
}

void Trace::OnCoroutineMigrate(Coroutine* coroutine, ProcesserId from_processer_id, ProcesserId to_processer_id)
{
#ifndef BBT_COROUTINE_TRACE
    (void)coroutine;
    (void)from_processer_id;
    (void)to_processer_id;
#else
    if (coroutine == nullptr || !coroutine->IsTraceEnabled())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_active_records.find(coroutine->GetId());
    if (it == m_active_records.end())
        return;

    _SyncMeta(it->second, coroutine, true);
    _PushEvent(it->second, TraceEventKind::MIGRATE, coroutine, to_processer_id, BBT_COROUTINE_INVALID_COPOLLEVENT_ID, static_cast<int>(from_processer_id));
#endif
}

void Trace::OnEventRegister(CoroutineId id, CoPollEventId event_id, int event, int custom_key)
{
#ifndef BBT_COROUTINE_TRACE
    (void)id;
    (void)event_id;
    (void)event;
    (void)custom_key;
#else
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_active_records.find(id);
    if (it == m_active_records.end())
        return;

    _PushEventById(it->second, TraceEventKind::EVENT_REGISTER, id, event_id, event | (custom_key << 16));
#endif
}

void Trace::OnEventTrigger(CoroutineId id, CoPollEventId event_id, int event, int custom_key)
{
#ifndef BBT_COROUTINE_TRACE
    (void)id;
    (void)event_id;
    (void)event;
    (void)custom_key;
#else
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_active_records.find(id);
    if (it == m_active_records.end())
        return;

    _PushEventById(it->second, TraceEventKind::EVENT_TRIGGER, id, event_id, event | (custom_key << 16));
#endif
}

CoroutineTraceSnapshot Trace::QueryCoroutine(CoroutineId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    CoroutineTraceSnapshot snapshot;

    auto active_it = m_active_records.find(id);
    if (active_it != m_active_records.end()) {
        snapshot.found = true;
        snapshot.active = active_it->second.active;
        snapshot.meta = active_it->second.meta;
        snapshot.recent_events.assign(active_it->second.recent_events.begin(), active_it->second.recent_events.end());
        return snapshot;
    }

    auto history_it = m_history_records.find(id);
    if (history_it != m_history_records.end()) {
        snapshot.found = true;
        snapshot.active = history_it->second.active;
        snapshot.meta = history_it->second.meta;
        snapshot.recent_events.assign(history_it->second.recent_events.begin(), history_it->second.recent_events.end());
    }

    return snapshot;
}

std::vector<CoroutineTraceMeta> Trace::QueryActiveCoroutinesByDesc(const std::string& desc, bool prefix)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<CoroutineTraceMeta> result;

    for (const auto& item : m_active_records) {
        const auto& meta = item.second.meta;
        const bool matched = prefix ? (meta.desc.rfind(desc, 0) == 0) : (meta.desc == desc);
        if (matched)
            result.push_back(meta);
    }

    return result;
}

std::string Trace::_FormatEvent(const CoroutineTraceEvent& event) const
{
    std::ostringstream oss;
    oss << "ts=" << event.timestamp_us
        << " kind=" << static_cast<int>(event.kind)
        << " status=" << static_cast<int>(event.status)
        << " proc=" << event.processer_id
        << " event=" << event.poll_event_id
        << " detail=" << event.detail_value;
    return oss.str();
}

std::string Trace::DumpCoroutine(CoroutineId id)
{
    auto snapshot = QueryCoroutine(id);
    if (!snapshot.found)
        return "trace not found";

    std::ostringstream oss;
    oss << "co=" << snapshot.meta.id
        << " desc=" << snapshot.meta.desc
        << " status=" << static_cast<int>(snapshot.meta.status)
        << " last_reason=" << snapshot.meta.last_resume_reason
        << " last_event=" << snapshot.meta.last_resume_event
        << " last_proc=" << snapshot.meta.last_processer_id
        << "\n";

    for (const auto& event : snapshot.recent_events)
        oss << _FormatEvent(event) << "\n";

    return oss.str();
}

} // namespace bbt::coroutine::detail
