// Standalone test for IDRScheduler with a fake clock. Run via tests/cpp/run_idr_scheduler_test.sh.
#include "alvr_server/IDRScheduler.h"

#include <cstdio>

static int g_failures = 0;
static uint64_t g_now = 1000 * 1000;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static const uint64_t MS = 1000;

static void configure(IDRScheduler& s) { s.SetMinIntervalUs(100 * MS); }

static void test_start_fires_on_next_frame() {
    IDRScheduler s([] { return g_now; });
    configure(s);
    s.OnStreamStart();
    CHECK(s.CheckIDRInsertion());
    CHECK(!s.CheckIDRInsertion());
}

static void test_first_request_after_quiet_fires_immediately() {
    IDRScheduler s([] { return g_now; });
    configure(s);
    s.OnStreamStart();
    CHECK(s.CheckIDRInsertion());
    g_now += 5000 * MS;
    s.InsertIDR();
    CHECK(s.CheckIDRInsertion());
}

static void test_burst_coalesces_to_boundary() {
    IDRScheduler s([] { return g_now; });
    configure(s);
    s.OnStreamStart();
    CHECK(s.CheckIDRInsertion()); // IDR at t0
    uint64_t t0 = g_now;
    g_now = t0 + 10 * MS;
    s.InsertIDR(); // e.g. "Decoder saturation" 10 ms after start
    s.OnPacketLoss();
    g_now = t0 + 11 * MS;
    CHECK(!s.CheckIDRInsertion()); // inside the interval: held, not fired
    g_now = t0 + 99 * MS;
    CHECK(!s.CheckIDRInsertion());
    g_now = t0 + 100 * MS;
    CHECK(s.CheckIDRInsertion());  // one IDR at the boundary, never dropped
    CHECK(!s.CheckIDRInsertion()); // the burst made exactly one
}

static void test_interval_restarts_after_boundary_idr() {
    IDRScheduler s([] { return g_now; });
    configure(s);
    s.OnStreamStart();
    CHECK(s.CheckIDRInsertion());
    uint64_t t0 = g_now;
    g_now = t0 + 10 * MS;
    s.InsertIDR();
    g_now = t0 + 100 * MS;
    CHECK(s.CheckIDRInsertion());
    g_now = t0 + 110 * MS;
    s.InsertIDR();
    g_now = t0 + 199 * MS;
    CHECK(!s.CheckIDRInsertion());
    g_now = t0 + 200 * MS;
    CHECK(s.CheckIDRInsertion());
}

static void test_restart_resets_interval() {
    IDRScheduler s([] { return g_now; });
    configure(s);
    s.OnStreamStart();
    CHECK(s.CheckIDRInsertion());
    g_now += 10 * MS;
    s.OnStreamStart(); // reconnect: start IDR fires at once
    CHECK(s.CheckIDRInsertion());
}

static void test_clock_stepping_backwards() {
    IDRScheduler s([] { return g_now; });
    configure(s);
    s.OnStreamStart();
    CHECK(s.CheckIDRInsertion());
    g_now -= 500 * MS; // wall clock steps back 500 ms
    s.InsertIDR();
    CHECK(s.CheckIDRInsertion()); // must not wait for the old timeline
    g_now += 10 * MS;
    s.InsertIDR();
    CHECK(!s.CheckIDRInsertion()); // interval is measured from the new timeline
    g_now += 90 * MS;
    CHECK(s.CheckIDRInsertion());
}

int main() {
    test_start_fires_on_next_frame();
    test_first_request_after_quiet_fires_immediately();
    test_burst_coalesces_to_boundary();
    test_interval_restarts_after_boundary_idr();
    test_restart_resets_interval();
    test_clock_stepping_backwards();
    std::printf(g_failures ? "FAILED (%d)\n" : "OK\n", g_failures);
    return g_failures ? 1 : 0;
}
