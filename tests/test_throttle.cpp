#include <QtTest>

#include "../src/services/scanner/ThrottlePolicy.h"

using usa::scanner::ThrottleInput;
using usa::scanner::ThrottlePolicy;

// Pure hysteresis logic: engage/release counts, flapping immunity,
// unsupported samplers, unknown latency. No threads, no OS.
class TestThrottlePolicy : public QObject {
    Q_OBJECT
private slots:
    void engagesAfterThreeBad()
    {
        ThrottlePolicy policy{};
        ThrottleInput bad{};
        bad.available = true;
        bad.latencyMs = 250.0;
        QVERIFY(!policy.feed(bad));
        QVERIFY(!policy.feed(bad));
        QVERIFY(policy.feed(bad)); // third consecutive: engaged
        QVERIFY(policy.throttled());
    }

    void singleGoodBreaksTheStreak()
    {
        ThrottlePolicy policy{};
        ThrottleInput bad{};
        bad.available = true;
        bad.latencyMs = 300.0;
        ThrottleInput good{};
        good.available = true;
        good.latencyMs = 5.0;
        QVERIFY(!policy.feed(bad));
        QVERIFY(!policy.feed(bad));
        QVERIFY(!policy.feed(good)); // streak broken
        QVERIFY(!policy.feed(bad));
        QVERIFY(!policy.feed(bad));
        QVERIFY(!policy.throttled()); // still needs a third consecutive
    }

    void releasesAfterTenClean()
    {
        ThrottlePolicy policy{};
        ThrottleInput bad{};
        bad.available = true;
        bad.queueDepth = 8.0;
        bad.activePct = 95.0;
        for (int i{0}; i < 3; ++i) {
            policy.feed(bad);
        }
        QVERIFY(policy.throttled());
        ThrottleInput good{};
        good.available = true;
        for (int i{0}; i < 9; ++i) {
            policy.feed(good);
            QVERIFY(policy.throttled()); // still held
        }
        policy.feed(good); // tenth consecutive clean sample releases
        QVERIFY(!policy.throttled());
    }

    void unavailableNeverThrottles()
    {
        ThrottlePolicy policy{};
        ThrottleInput in{};
        in.available = false;
        in.latencyMs = 99999.0; // ignored without availability
        for (int i{0}; i < 20; ++i) {
            QVERIFY(!policy.feed(in));
        }
    }

    void unknownLatencyUsesQueueAndActive()
    {
        ThrottlePolicy policy{};
        ThrottleInput in{};
        in.available = true;
        in.latencyMs = -1.0; // backends without latency counters
        in.queueDepth = 9.0;
        in.activePct = 99.0;
        QVERIFY(!policy.feed(in));
        QVERIFY(!policy.feed(in));
        QVERIFY(policy.feed(in));
    }

    void resetClears()
    {
        ThrottlePolicy policy{};
        ThrottleInput bad{};
        bad.available = true;
        bad.latencyMs = 500.0;
        for (int i{0}; i < 3; ++i) {
            policy.feed(bad);
        }
        QVERIFY(policy.throttled());
        policy.reset();
        QVERIFY(!policy.throttled());
    }
};

QTEST_MAIN(TestThrottlePolicy)
#include "test_throttle.moc"
