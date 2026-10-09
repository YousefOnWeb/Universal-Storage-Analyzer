#pragma once

namespace usa::scanner {

// Hysteresis policy for disk-throttle decisions: pure logic, no threads,
// no OS calls, unit-tested everywhere. The sampler feeds one struct per
// poll; the policy answers throttled or not. Flapping is impossible by
// construction: engaging takes N consecutive bad samples, releasing takes
// M consecutive clean ones, and the counters reset each other.
struct ThrottleInput {
    bool available{false}; // sampler unsupported: never throttles
    double latencyMs{-1.0}; // average transfer latency; <0 when unknown
    double queueDepth{0.0}; // average queue length
    double activePct{0.0}; // disk active time percent, 0-100
};

class ThrottlePolicy {
public:
    static constexpr double kLatencyMs{200.0};
    static constexpr double kQueueDepth{4.0};
    static constexpr double kActivePct{90.0};
    static constexpr int kEngageAfter{3};
    static constexpr int kReleaseAfter{10};

    ThrottlePolicy() = default;

    void reset()
    {
        bad_ = 0;
        good_ = 0;
        throttled_ = false;
    }

    bool throttled() const { return throttled_; }

    // Returns the new throttled state (compare with throttled() before
    // the call to detect transitions for status display).
    bool feed(const ThrottleInput& in)
    {
        const bool bad = in.available
                         && (in.latencyMs > kLatencyMs
                             || (in.queueDepth > kQueueDepth && in.activePct > kActivePct));
        if (bad) {
            ++bad_;
            good_ = 0;
            if (bad_ >= kEngageAfter) {
                throttled_ = true;
            }
        } else {
            ++good_;
            bad_ = 0;
            if (good_ >= kReleaseAfter) {
                throttled_ = false;
            }
        }
        return throttled_;
    }

private:
    int bad_{0};
    int good_{0};
    bool throttled_{false};
};

} // namespace usa::scanner
