#include "IDRScheduler.h"

#include <algorithm>
#include <mutex>

#ifndef IDR_SCHEDULER_STANDALONE
#include "Utils.h"
#endif

IDRScheduler::IDRScheduler(std::function<uint64_t()> clock) : m_clock(std::move(clock)) {
#ifndef IDR_SCHEDULER_STANDALONE
    if (!m_clock) {
        m_clock = GetTimestampUs;
    }
#endif
}

IDRScheduler::~IDRScheduler() { }

void IDRScheduler::OnPacketLoss() { InsertIDR(); }

void IDRScheduler::OnStreamStart() {
#ifndef IDR_SCHEDULER_STANDALONE
    m_minIDRFrameInterval = Settings::Instance().m_minimumIdrIntervalMs * 1000;
#endif
    m_scheduled = false;
    m_hasLastIDR = false;
    InsertIDR();
}

void IDRScheduler::InsertIDR() {
    std::unique_lock lock(m_mutex);

    // A pending IDR already covers this request (coalesced).
    if (m_scheduled) {
        return;
    }

    uint64_t now = m_clock();
    m_insertIDRTime = m_hasLastIDR ? std::max(now, m_lastIDRTime + m_minIDRFrameInterval) : now;
    m_scheduled = true;
}

bool IDRScheduler::CheckIDRInsertion() {
    std::unique_lock lock(m_mutex);

    if (m_scheduled) {
        uint64_t now = m_clock();
        if (m_insertIDRTime <= now) {
            m_scheduled = false;
            m_hasLastIDR = true;
            m_lastIDRTime = now;
            return true;
        }
    }
    return false;
}
