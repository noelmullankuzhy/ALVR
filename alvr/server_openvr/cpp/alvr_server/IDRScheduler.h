#pragma once

#ifndef IDR_SCHEDULER_STANDALONE
#include "Settings.h"
#endif
#include <functional>
#include <mutex>
#include <stdint.h>

class IDRScheduler {
public:
    // The clock returns microseconds; it defaults to GetTimestampUs.
    explicit IDRScheduler(std::function<uint64_t()> clock = nullptr);
    ~IDRScheduler();

    void OnPacketLoss();

    void OnStreamStart();
    void InsertIDR();

    bool CheckIDRInsertion();

    void SetMinIntervalUs(uint64_t minIntervalUs) { m_minIDRFrameInterval = minIntervalUs; }

private:
    static const int MIN_IDR_FRAME_INTERVAL = 100 * 1000; // 100-milliseconds
    std::function<uint64_t()> m_clock;
    uint64_t m_insertIDRTime = 0;
    uint64_t m_lastIDRTime = 0;
    bool m_hasLastIDR = false;
    bool m_scheduled = false;
    std::mutex m_mutex;
    uint64_t m_minIDRFrameInterval = MIN_IDR_FRAME_INTERVAL;
};
