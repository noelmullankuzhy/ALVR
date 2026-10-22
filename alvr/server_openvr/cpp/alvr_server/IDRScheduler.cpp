#include "IDRScheduler.h"

#include <algorithm>
#include <chrono>
#include <mutex>

// Monotonic, so a wall-clock step cannot stretch the interval.
static uint64_t SteadyTimestampUs() {
    auto duration = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

IDRScheduler::IDRScheduler(std::function<uint64_t()> clock) : m_clock(std::move(clock)) {
    if (!m_clock) {
        m_clock = SteadyTimestampUs;
    }
}

IDRScheduler::~IDRScheduler() { }

void IDRScheduler::OnPacketLoss() { InsertIDR(); }

void IDRScheduler::OnStreamStart() {
    std::unique_lock lock(m_mutex);

#ifndef IDR_SCHEDULER_STANDALONE
    m_minIDRFrameInterval = Settings::Instance().m_minimumIdrIntervalMs * 1000;
#endif
    m_scheduled = false;
    m_hasLastIDR = false;
    ScheduleLocked();
}

void IDRScheduler::InsertIDR() {
    std::unique_lock lock(m_mutex);

    ScheduleLocked();
}

void IDRScheduler::ScheduleLocked() {
    // A pending IDR already covers this request (coalesced).
    if (m_scheduled) {
        return;
    }

    uint64_t now = m_clock();
    // now < last means the clock stepped back: treat it as no previous IDR.
    bool hasLast = m_hasLastIDR && now >= m_lastIDRTime;
    m_insertIDRTime = hasLast ? std::max(now, m_lastIDRTime + m_minIDRFrameInterval) : now;
    m_scheduled = true;
}

bool IDRScheduler::CheckIDRInsertion() {
    std::unique_lock lock(m_mutex);

    if (m_scheduled) {
        uint64_t now = m_clock();
        // A due time further out than one interval can only come from a clock step back.
        if (m_insertIDRTime > now + m_minIDRFrameInterval) {
            m_insertIDRTime = now;
        }
        if (m_insertIDRTime <= now) {
            m_scheduled = false;
            m_hasLastIDR = true;
            m_lastIDRTime = now;
            return true;
        }
    }
    return false;
}
