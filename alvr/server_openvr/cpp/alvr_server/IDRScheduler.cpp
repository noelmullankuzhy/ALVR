#include "IDRScheduler.h"

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
    InsertIDR();
}

void IDRScheduler::InsertIDR() {
    std::unique_lock lock(m_mutex);

    m_insertIDRTime = m_clock() - MIN_IDR_FRAME_INTERVAL * 2;
    m_scheduled = true;
}

bool IDRScheduler::CheckIDRInsertion() {
    std::unique_lock lock(m_mutex);

    if (m_scheduled) {
        if (m_insertIDRTime <= m_clock()) {
            m_scheduled = false;
            return true;
        }
    }
    return false;
}
