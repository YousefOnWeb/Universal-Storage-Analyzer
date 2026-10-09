#include <QtTest>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include "../src/services/DiskTelemetry.h"

// Sampler lifecycle on every OS: monitoring starts, periodic samples
// arrive within the poll window, stopping ends them. Field sanity is
// asserted only when the platform reports availability.
class TestDiskTelemetry : public QObject {
    Q_OBJECT
private slots:
    void emitsPeriodicSamples()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        DiskTelemetry telemetry{};
        QSignalSpy samples{&telemetry, &DiskTelemetry::sampleReady};
        telemetry.startMonitoring(tmp.path());
        QVERIFY(samples.wait(3000)); // 1 Hz poll: first sample lands fast
        QTest::qWait(1200); // room for a second tick
        telemetry.stopMonitoring();
        const int count = samples.count();
        QVERIFY(count >= 1);
        for (const auto& args : samples) {
            const usa::DiskSample sample = args.at(0).value<usa::DiskSample>();
            QVERIFY(sample.queueDepth >= 0.0);
            QVERIFY(sample.activePct >= 0.0 && sample.activePct <= 100.0);
            if (sample.available) {
                QVERIFY(sample.latencyMs >= 0.0);
            }
        }
        // Stopped: no further samples arrive.
        QTest::qWait(1200);
        QCOMPARE(samples.count(), count);
    }

    void restartReplacesTarget()
    {
        QTemporaryDir first{};
        QTemporaryDir second{};
        QVERIFY(first.isValid());
        QVERIFY(second.isValid());
        DiskTelemetry telemetry{};
        QSignalSpy samples{&telemetry, &DiskTelemetry::sampleReady};
        telemetry.startMonitoring(first.path());
        QVERIFY(samples.wait(3000));
        telemetry.startMonitoring(second.path()); // restart: no crash, no leak
        QVERIFY(samples.wait(3000));
        telemetry.stopMonitoring();
    }
};

QTEST_MAIN(TestDiskTelemetry)
#include "test_telemetry.moc"
