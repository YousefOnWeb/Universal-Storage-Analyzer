#include <QtTest>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../src/services/scanner/BlockingQueue.h"

using usa::scanner::BlockingQueue;

class TestBlockingQueue : public QObject {
    Q_OBJECT
private slots:
    void preservesFifoOrder()
    {
        BlockingQueue<int> q{};
        q.push(1);
        q.push(2);
        q.push(3);
        int out{0};
        QVERIFY(q.pop(out));
        QCOMPARE(out, 1);
        q.taskDone();
        QVERIFY(q.pop(out));
        QCOMPARE(out, 2);
        q.taskDone();
        QVERIFY(q.pop(out));
        QCOMPARE(out, 3);
        q.taskDone();
        q.waitUntilIdle(); // returns immediately; would hang on imbalance
    }

    void popBlocksUntilPush()
    {
        BlockingQueue<int> q{};
        int got{-1};
        std::thread consumer{[&] {
            int v{0};
            if (q.pop(v)) {
                got = v;
                q.taskDone();
            }
        }};
        QTRY_VERIFY_WITH_TIMEOUT(q.waitingWorkers() == 1, 2000);
        q.push(42);
        consumer.join();
        QCOMPARE(got, 42);
    }

    void cancelUnblocksPop()
    {
        BlockingQueue<int> q{};
        bool result{true};
        int dummy{0};
        std::thread consumer{[&] { result = q.pop(dummy); }};
        QTRY_VERIFY_WITH_TIMEOUT(q.waitingWorkers() == 1, 2000);
        q.requestCancel();
        consumer.join();
        QVERIFY(!result);
        QVERIFY(q.isCancelRequested());
    }

    void suspendPausesDelivery()
    {
        BlockingQueue<int> q{};
        q.setSuspended(true);
        q.push(7);
        int out{0};
        QVERIFY(!q.tryPop(out));
        q.setSuspended(false);
        QVERIFY(q.tryPop(out));
        QCOMPARE(out, 7);
        q.taskDone();
    }

    void waitIfSuspendedUnblocksOnResumeOrCancel()
    {
        BlockingQueue<int> q{};
        q.setSuspended(true);
        std::atomic_bool proceeded{false};
        std::thread waiter{[&] {
            q.waitIfSuspended();
            proceeded = !q.isCancelRequested();
        }};
        QTest::qWait(50);
        QVERIFY(!proceeded.load());
        q.setSuspended(false);
        waiter.join();
        QVERIFY(proceeded.load());
    }

    void stressMultiProducerConsumer()
    {
        BlockingQueue<int> q{};
        constexpr int kProducers{4};
        constexpr int kEach{250};
        std::atomic<long long> sum{0};
        std::vector<std::thread> consumers{};
        for (int c{0}; c < 4; ++c) {
            consumers.emplace_back([&] {
                int v{0};
                while (q.pop(v)) {
                    sum.fetch_add(v, std::memory_order_relaxed);
                    q.taskDone();
                }
            });
        }
        std::vector<std::thread> producers{};
        for (int p{0}; p < kProducers; ++p) {
            producers.emplace_back([&, p] {
                for (int i{0}; i < kEach; ++i) {
                    q.push(p * kEach + i);
                }
            });
        }
        for (auto& t : producers) {
            t.join();
        }
        q.waitUntilIdle();
        q.requestCancel();
        for (auto& t : consumers) {
            t.join();
        }
        // 0 + 1 + ... + 999
        QCOMPARE(sum.load(), 999LL * 1000LL / 2LL);
    }
};

QTEST_MAIN(TestBlockingQueue)
#include "test_blockingqueue.moc"
