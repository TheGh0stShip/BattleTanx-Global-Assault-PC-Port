#include "btga/bootstrap_recovery.hpp"
#include "btga/client_resync.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            std::cerr << "check failed at " << __FILE__ << ':' << __LINE__ \
                      << ": " #expression "\n"; \
            return 1; \
        } \
    } while (false)

int main() {
    using Request = btga::BootstrapRequest;
    using Ack = btga::BootstrapAckKind;
    btga::BootstrapRecovery recovery;

    {
        using namespace std::chrono_literals;
        using Decision = btga::ClientResyncDecision;
        using Clock = btga::ClientResyncPolicy::Clock;
        btga::ClientResyncPolicy policy;
        const auto start = Clock::time_point{1000s};

        CHECK(policy.recent_attempts(start) == 0U);
        CHECK(policy.mismatch(start) == Decision::request_checkpoint);
        CHECK(policy.mismatch(start + 30s) == Decision::request_checkpoint);
        CHECK(policy.mismatch(start + 60s) == Decision::request_checkpoint);
        CHECK(policy.recent_attempts(start + 60s) == 3U);
        // A persistently incompatible client is isolated before it can acquire
        // the authority's shared recovery barrier for a fourth time.
        CHECK(policy.mismatch(start + 90s) == Decision::quarantine);
        CHECK(policy.recent_attempts(start + 90s) == 3U);
        // The sliding window permits recovery again after the oldest failure
        // ages out; a clean session reset clears the history immediately.
        CHECK(policy.mismatch(start + 5min) == Decision::request_checkpoint);
        CHECK(policy.recent_attempts(start + 5min) == 3U);
        policy.reset();
        CHECK(policy.recent_attempts(start + 5min) == 0U);

        btga::ClientInputLiveness liveness;
        liveness.observe_transport(0U, start);
        CHECK(liveness.seen(0U));
        CHECK(!liveness.input_stale(0U, start + 2s, 2s));
        // Transport remains healthy, but it cannot refresh input progress.
        liveness.observe_transport(0U, start + 3s);
        CHECK(liveness.input_stale(0U, start + 3s, 2s));
        CHECK(!liveness.transport_stale(0U, start + 3s, 2s));

        CHECK(liveness.observe_input(0U, 100U, start + 3s));
        CHECK(!liveness.input_stale(0U, start + 5s, 2s));
        // Duplicate and older horizons do not sustain held controls.
        CHECK(!liveness.observe_input(0U, 100U, start + 6s));
        CHECK(!liveness.observe_input(0U, 99U, start + 6s));
        CHECK(liveness.input_stale(0U, start + 6s, 2s));
        CHECK(liveness.observe_input(0U, 101U, start + 6s));

        // Serial arithmetic accepts genuine progress through uint32 wrap.
        liveness.clear(1U);
        CHECK(liveness.observe_input(1U, UINT32_MAX - 1U, start));
        CHECK(liveness.observe_input(1U, 1U, start + 1s));
        CHECK(!liveness.observe_input(1U, UINT32_MAX, start + 2s));
        liveness.clear(0U);
        CHECK(!liveness.seen(0U));
        liveness.observe_transport(4U, start);
        CHECK(!liveness.seen(4U));
    }

    {
        using namespace std::chrono_literals;
        using Clock = btga::RecoveryProgressWatchdog::Clock;
        btga::RecoveryProgressWatchdog watchdog;
        const auto start = Clock::time_point{100s};

        CHECK(!watchdog.stalled(0U, start + 1h, 15s));
        watchdog.observe(0U, 20U, start);
        CHECK(watchdog.next_tick(0U) == 20U);
        CHECK(!watchdog.stalled(0U, start + 15s, 15s));
        CHECK(watchdog.stalled(0U, start + 15s + 1ms, 15s));

        // Duplicate and backwards acknowledgments cannot keep a broken peer's
        // deadline alive. Genuine forward progress refreshes it.
        watchdog.observe(0U, 20U, start + 14s);
        watchdog.observe(0U, 19U, start + 14s);
        CHECK(watchdog.stalled(0U, start + 16s, 15s));
        watchdog.observe(0U, 21U, start + 16s);
        CHECK(!watchdog.stalled(0U, start + 30s, 15s));

        // Tick comparison follows the protocol's uint32 serial arithmetic.
        watchdog.observe(1U, UINT32_MAX - 1U, start);
        watchdog.observe(1U, 1U, start + 1s);
        CHECK(watchdog.next_tick(1U) == 1U);
        CHECK(!watchdog.stalled(1U, start + 15s, 15s));

        watchdog.clear(0U);
        CHECK(!watchdog.stalled(0U, start + 1h, 15s));
        watchdog.observe(4U, 99U, start);
        CHECK(watchdog.next_tick(4U) == 0U);
        watchdog.reset();
        CHECK(watchdog.next_tick(1U) == 0U);
    }

    // Invalid external seat values cannot alter the four-seat state machine.
    CHECK(recovery.request_cold(4U, 90U) == Request::continue_generation);
    recovery.snapshot_sent(4U, 1U, 100U);
    CHECK(recovery.acknowledge(4U, 101U, 1U, 100U, true).kind == Ack::ignore);
    CHECK(!recovery.paused() && recovery.mask() == 0U);

    // A delayed send completion cannot resurrect a disconnected or otherwise
    // unrequested seat.
    recovery.snapshot_sent(0U, 77U, 50U);
    CHECK(!recovery.pending(0U));
    CHECK(recovery.snapshot_id(0U) == 0U);

    // Every seat can recover concurrently. Snapshot installation at checkpoint
    // 90 is insufficient; each seat independently holds the shared barrier
    // until it has replayed through head 100.
    for (std::uint8_t seat = 0; seat < 4U; ++seat) {
        CHECK(recovery.request_cold(seat, 90U) ==
              Request::start_generation);
        recovery.snapshot_sent(seat, 1000U + seat, 100U);
    }
    // Snapshot requests alone cannot freeze healthy players. The barrier is
    // acquired only when each endpoint proves installation of its generation.
    CHECK(recovery.mask() == 0U);
    for (std::uint8_t seat = 0; seat < 4U; ++seat) {
        const auto stale = recovery.acknowledge(
            seat, 101U, 999U, 100U, false);
        CHECK(stale.kind == Ack::ignore);
        const auto installed = recovery.acknowledge(
            seat, 91U, 1000U + seat, 100U, false);
        CHECK(installed.kind == Ack::replay && installed.next_tick == 90U);
    }
    CHECK(recovery.mask() == 0x0fU);
    for (std::uint8_t seat = 0; seat < 4U; ++seat) {
        const auto ready = recovery.acknowledge(
            seat, 101U, 1000U + seat, 100U, false);
        CHECK(ready.kind == Ack::ready && ready.next_tick == 100U);
        // Earlier seats have already cleared too.
        const auto remaining = static_cast<std::uint8_t>(
            0x0fU & ~((1U << (seat + 1U)) - 1U));
        CHECK(recovery.mask() == remaining);
    }
    CHECK(!recovery.paused());

    // Every ordering of four simultaneous installations and completions must
    // produce the same aggregate barrier. This catches accidental coupling to
    // the round-robin snapshot transport order.
    std::array<std::uint8_t, 4> order{0U, 1U, 2U, 3U};
    do {
        recovery.reset();
        for (const auto seat : order) {
            CHECK(recovery.request_cold(seat, 80U) ==
                  Request::start_generation);
            recovery.snapshot_sent(seat, 5000U + seat, 100U);
        }
        CHECK(recovery.mask() == 0U);
        std::uint8_t installed_mask = 0U;
        for (const auto seat : order) {
            CHECK(recovery.acknowledge(
                      seat, 81U, 5000U + seat, 100U, false).kind ==
                  Ack::replay);
            installed_mask = static_cast<std::uint8_t>(
                installed_mask | (1U << seat));
            CHECK(recovery.mask() == installed_mask);
        }
        for (auto iterator = order.rbegin(); iterator != order.rend(); ++iterator) {
            const auto seat = *iterator;
            CHECK(recovery.acknowledge(
                      seat, 101U, 5000U + seat, 100U, false).kind ==
                  Ack::ready);
            installed_mask = static_cast<std::uint8_t>(
                installed_mask & ~(1U << seat));
            CHECK(recovery.mask() == installed_mask);
        }
        CHECK(!recovery.paused());
    } while (std::next_permutation(order.begin(), order.end()));
    recovery.reset();

    // Neither a pending recovery nor an ordinary subscription may claim a
    // future tick.  A future acknowledgment used to clear the barrier and
    // resume simulation even though the client had replayed nothing.
    CHECK(recovery.request_cold(0U, 120U) ==
          Request::start_generation);
    recovery.snapshot_sent(0U, 1500U, 150U);
    CHECK(recovery.acknowledge(0U, 152U, 1500U, 150U, false).kind ==
          Ack::ignore);
    CHECK(recovery.pending(0U));
    CHECK(recovery.mask() == 0U);
    CHECK(recovery.expected_ack(0U) == 151U);
    CHECK(recovery.acknowledge(0U, 151U, 1500U, 150U, false).kind ==
          Ack::ready);
    CHECK(!recovery.paused());

    CHECK(recovery.acknowledge(3U, 202U, 0U, 200U, true).kind ==
          Ack::ignore);
    CHECK(recovery.mask() == 0U);
    CHECK(recovery.acknowledge(3U, 201U, 0U, 200U, true).kind ==
          Ack::ready);
    CHECK(recovery.mask() == 0U);

    // Repeated ack-zero datagrams caused by packet loss cannot churn a positive
    // generation or move its target head.
    CHECK(recovery.request_cold(0U, 180U) == Request::start_generation);
    recovery.snapshot_sent(0U, 2000U, 200U);
    for (unsigned retry = 0; retry < 1000U; ++retry) {
        CHECK(recovery.request_cold(0U, 180U) ==
              Request::continue_generation);
        CHECK(recovery.snapshot_id(0U) == 2000U);
        CHECK(recovery.expected_ack(0U) == 201U);
    }
    recovery.disconnect(0U);
    CHECK(!recovery.pending(0U) && !recovery.paused());

    // An unfinished initial generation is the one permitted supersession: it
    // upgrades once when a positive checkpoint appears, then remains stable.
    CHECK(recovery.request_cold(1U, 0U) == Request::start_generation);
    recovery.snapshot_sent(1U, 3000U, 0U);
    CHECK(recovery.request_cold(1U, 30U) ==
          Request::supersede_initial_generation);
    CHECK(recovery.snapshot_id(1U) == 0U && recovery.mask() == 0U);
    recovery.snapshot_sent(1U, 3001U, 30U);
    CHECK(recovery.request_cold(1U, 30U) ==
          Request::continue_generation);
    CHECK(recovery.snapshot_id(1U) == 3001U);
    CHECK(recovery.acknowledge(1U, 31U, 3000U, 30U, false).kind == Ack::ignore);
    CHECK(recovery.acknowledge(1U, 31U, 3001U, 30U, false).kind == Ack::ready);
    CHECK(!recovery.paused());

    // A valid tick-zero install that arrives after the world advances must
    // open a replay barrier even without a new subscription event.
    CHECK(recovery.request_cold(2U, 0U) == Request::start_generation);
    recovery.snapshot_sent(2U, 4000U, 0U);
    const auto late = recovery.acknowledge(2U, 1U, 4000U, 12U, false);
    CHECK(late.kind == Ack::replay && recovery.mask() == 0x04U);
    CHECK(recovery.acknowledge(2U, 13U, 4000U, 12U, false).kind == Ack::ready);
    CHECK(!recovery.paused());

    return 0;
}
