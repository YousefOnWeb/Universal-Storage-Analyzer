#include <QtTest>

#include <cstdint>
#include <string>
#include <vector>

#include "../src/services/scanner/Analytics.h"

using usa::scanner::Analytics;

// Pure accumulator logic: extension parsing, bounded top-N heap, age
// buckets, owner grouping. No threads, no filesystem, no Qt.
class TestAnalytics : public QObject {
    Q_OBJECT
private slots:
    void parsesExtensions()
    {
        using A = usa::scanner::Analytics;
        QCOMPARE(QString::fromStdString(A::extensionOf("clip.MP4")), QStringLiteral(".mp4"));
        QCOMPARE(QString::fromStdString(A::extensionOf("archive.tar.gz")),
                 QStringLiteral(".gz"));
        QCOMPARE(QString::fromStdString(A::extensionOf("noext")),
                 QStringLiteral("(no extension)"));
        QCOMPARE(QString::fromStdString(A::extensionOf(".bashrc")),
                 QStringLiteral("(no extension)"));
        QCOMPARE(QString::fromStdString(A::extensionOf("trailing.")),
                 QStringLiteral("(no extension)"));
        QCOMPARE(QString::fromStdString(A::extensionOf("")), QStringLiteral("(no extension)"));
    }

    void aggregatesExtensionsCaseInsensitively()
    {
        Analytics a{};
        a.recordFile(7, "a.LOG", "/a.LOG", 100, 0, true, {});
        a.recordFile(7, "b.log", "/b.log", 200, 0, true, {});
        a.recordFile(7, "c.mp4", "/c.mp4", 300, 0, true, {});
        a.recordFile(7, "noext", "/noext", 50, 0, true, {});
        const auto snap = a.snapshot();
        QCOMPARE(snap.extensions.size(), static_cast<std::size_t>(3));
        // Sorted by bytes desc: .log (300), .mp4 (300)... tie order
        // unspecified, so look up.
        std::uint64_t logBytes{0};
        std::uint64_t logFiles{0};
        for (const auto& [ext, st] : snap.extensions) {
            if (ext == ".log") {
                logBytes = st.bytes;
                logFiles = st.files;
            }
        }
        QCOMPARE(logBytes, static_cast<std::uint64_t>(300));
        QCOMPARE(logFiles, static_cast<std::uint64_t>(2));
    }

    void topNKeepsLargestBounded()
    {
        Analytics a{};
        a.setTopN(2);
        a.recordFile(1, "small", "/small", 10, 0, true, {});
        a.recordFile(2, "big", "/big", 500, 0, true, {});
        a.recordFile(3, "mid", "/mid", 100, 0, true, {});
        const auto snap = a.snapshot();
        QCOMPARE(snap.top.size(), static_cast<std::size_t>(2));
        QCOMPARE(snap.top[0].bytes, static_cast<std::uint64_t>(500));
        QCOMPARE(snap.top[0].id, static_cast<std::uint32_t>(2));
        QCOMPARE(snap.top[1].bytes, static_cast<std::uint64_t>(100));
        QCOMPARE(QString::fromStdString(snap.top[0].pathUtf8), QStringLiteral("/big"));
    }

    void bucketsAgesAndSkipsUnknown()
    {
        Analytics a{};
        a.setAgeBuckets({7, 30});
        a.recordFile(7, "fresh", "/fresh", 10, 0, true, {}); // bucket 0
        a.recordFile(7, "week", "/week", 20, 10, true, {}); // bucket 1
        a.recordFile(7, "old", "/old", 40, 99, true, {}); // bucket 2
        a.recordFile(7, "mystery", "/mystery", 80, 0, false, {}); // skipped
        const auto snap = a.snapshot();
        QCOMPARE(snap.ageBytes.size(), static_cast<std::size_t>(3));
        QCOMPARE(snap.ageBytes[0], static_cast<std::uint64_t>(10));
        QCOMPARE(snap.ageBytes[1], static_cast<std::uint64_t>(20));
        QCOMPARE(snap.ageBytes[2], static_cast<std::uint64_t>(40));
        QCOMPARE(snap.ageFiles[0], static_cast<std::uint64_t>(1));
        QCOMPARE(snap.ageFiles[1], static_cast<std::uint64_t>(1));
        QCOMPARE(snap.ageFiles[2], static_cast<std::uint64_t>(1));
        // Edges ride along so panes label buckets without a second channel.
        QCOMPARE(snap.ageEdges.size(), static_cast<std::size_t>(2));
        QCOMPARE(snap.ageEdges[0], static_cast<std::uint64_t>(7));
        QCOMPARE(snap.ageEdges[1], static_cast<std::uint64_t>(30));
    }

    void groupsUnknownOwners()
    {
        Analytics a{};
        a.recordFile(7, "a", "/a", 100, 0, true, "1000");
        a.recordFile(7, "b", "/b", 200, 0, true, {});
        a.recordFile(7, "c", "/c", 50, 0, true, "1000");
        const auto snap = a.snapshot();
        QCOMPARE(snap.owners.size(), static_cast<std::size_t>(2));
        // Sorted desc: "(unknown)" (200) before "1000" (150).
        QCOMPARE(QString::fromStdString(snap.owners[0].first),
                 QStringLiteral("(unknown)"));
        QCOMPARE(snap.owners[0].second.bytes, static_cast<std::uint64_t>(200));
        QCOMPARE(QString::fromStdString(snap.owners[1].first), QStringLiteral("1000"));
        QCOMPARE(snap.owners[1].second.files, static_cast<std::uint64_t>(2));
    }

    void resetClearsAll()
    {
        Analytics a{};
        a.recordFile(7, "a.log", "/a.log", 100, 3, true, "7");
        a.reset();
        const auto snap = a.snapshot();
        QVERIFY(snap.extensions.empty());
        QVERIFY(snap.top.empty());
        QVERIFY(snap.owners.empty());
        for (const auto b : snap.ageBytes) {
            QCOMPARE(b, static_cast<std::uint64_t>(0));
        }
    }
};

QTEST_MAIN(TestAnalytics)
#include "test_analytics.moc"
