#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "../src/services/scanner/MftFinder.h"
#include "../src/services/scanner/ScanEngine.h"

using usa::scanner::EngineDirSummary;
using usa::scanner::EngineNode;
using usa::scanner::ScanEngine;

namespace {

void writeBytes(const std::filesystem::path& p, std::size_t n)
{
    std::ofstream f{p, std::ios::binary};
    const std::string data(n, 'x');
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

struct Collected {
    std::mutex mutex{};
    std::vector<EngineNode> nodes{};
    std::map<std::uint32_t, EngineDirSummary> lastSummary{};
    std::atomic<int> batches{0};
};

} // namespace

class TestScanEngine : public QObject {
    Q_OBJECT
private slots:
    void scansKnownTreeWithExactTotals()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        const std::filesystem::path root{tmp.path().toStdWString()};
        writeBytes(root / "a.bin", 100);
        writeBytes(root / "b.bin", 200);
        std::filesystem::create_directory(root / "sub");
        writeBytes(root / "sub" / "c.bin", 300);
        std::filesystem::create_directory(root / "emptydir");

        ScanEngine engine{};
        Collected out{};
        usa::scanner::EngineCallbacks cb{};
        cb.onBatch = [&](std::vector<EngineNode> batch) {
            ++out.batches;
            std::lock_guard<std::mutex> lock{out.mutex};
            for (auto& n : batch) {
                out.nodes.push_back(std::move(n));
            }
        };
        cb.onDirSummary = [&](const EngineDirSummary& s) {
            std::lock_guard<std::mutex> lock{out.mutex};
            out.lastSummary[s.nodeId] = s;
        };
        const auto result = engine.run(root, cb);

        QCOMPARE(result.fileCount, static_cast<std::uint64_t>(3));
        QCOMPARE(result.dirCount, static_cast<std::uint64_t>(2));
        QCOMPARE(result.totalBytes, static_cast<std::uint64_t>(600));
        QVERIFY(!result.cancelled);
        // Backend follows the OS gate: native where direct reading is
        // unavailable, direct where it is (elevated NTFS). Totals hold on
        // both paths; where the gate is closed the backend is pinned.
        if (!usa::scanner::isDirectScanAvailable(root)) {
            QCOMPARE(result.backend, usa::scanner::BackendKind::NativeDirectory);
        }
        QVERIFY(out.batches.load() >= 1);

        // 3 files + 2 directories; ids unique; parents resolve.
        QCOMPARE(out.nodes.size(), static_cast<std::size_t>(5));
        std::map<std::uint32_t, const EngineNode*> byId{{0, nullptr}};
        for (const auto& n : out.nodes) {
            QVERIFY(!byId.contains(n.id));
            byId[n.id] = &n;
        }
        for (const auto& n : out.nodes) {
            QVERIFY(byId.contains(n.parentId));
        }

        // Final per-directory summaries converge to exact subtree totals
        // (replace semantics — no doubling). Counts are subtree-wide too.
        QVERIFY(out.lastSummary.contains(0));
        QCOMPARE(out.lastSummary.at(0).logicalSize, static_cast<std::uint64_t>(600));
        QCOMPARE(out.lastSummary.at(0).fileCount, static_cast<std::uint32_t>(3));
        QCOMPARE(out.lastSummary.at(0).folderCount, static_cast<std::uint32_t>(2));
        std::uint32_t subId{0};
        for (const auto& n : out.nodes) {
            if (n.nameUtf8 == "sub") {
                subId = n.id;
            }
        }
        QVERIFY(subId != 0);
        QVERIFY(out.lastSummary.contains(subId));
        QCOMPARE(out.lastSummary.at(subId).logicalSize, static_cast<std::uint64_t>(300));
        QCOMPARE(out.lastSummary.at(subId).fileCount, static_cast<std::uint32_t>(1));

        // The final sweep covers every discovered directory, so all of
        // them converge no matter which intermediate summaries were
        // throttled into leaf-only reports.
        std::set<std::uint32_t> dirIds{0};
        for (const auto& n : out.nodes) {
            if (n.isDirectory) {
                dirIds.insert(n.id);
            }
        }
        QCOMPARE(dirIds.size(), static_cast<std::size_t>(3)); // root + sub + emptydir
        for (const std::uint32_t id : dirIds) {
            QVERIFY(out.lastSummary.contains(id));
        }
    }

    void pauseHoldsAndResumeCompletes()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        const std::filesystem::path root{tmp.path().toStdWString()};
        writeBytes(root / "a.bin", 10);

        ScanEngine engine{};
        engine.setPaused(true);
        std::atomic<int> seen{0};
        usa::scanner::EngineCallbacks cb{};
        cb.onBatch = [&](std::vector<EngineNode>) { ++seen; };
        usa::scanner::EngineResult result{};
        std::thread runner{[&] { result = engine.run(root, cb); }};
        QTest::qWait(200);
        QCOMPARE(seen.load(), 0); // nothing streams while paused
        engine.setPaused(false);
        runner.join();
        QVERIFY(!result.cancelled);
        QCOMPARE(result.fileCount, static_cast<std::uint64_t>(1));
        QVERIFY(seen.load() >= 1);
    }

    void cancelWhilePausedTerminates()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        const std::filesystem::path root{tmp.path().toStdWString()};
        writeBytes(root / "a.bin", 10);

        ScanEngine engine{};
        engine.setPaused(true); // deterministic: workers block before any work
        usa::scanner::EngineCallbacks cb{};
        usa::scanner::EngineResult result{};
        std::thread runner{[&] { result = engine.run(root, cb); }};
        QTest::qWait(100);
        engine.requestCancel();
        runner.join(); // must return; would hang without cancel-aware idle
        QVERIFY(result.cancelled);
    }

    void reportsMissingRoot()
    {
        ScanEngine engine{};
        bool sawError{false};
        bool sawFatal{false};
        usa::scanner::EngineCallbacks cb{};
        cb.onError = [&](const std::string&, bool fatal) {
            sawError = true;
            sawFatal = fatal;
        };
        const auto result = engine.run(std::filesystem::path{"Z:/definitely-not-here-usa-test"}, cb);
        QVERIFY(sawError);
        QVERIFY(sawFatal); // missing target stops the scan: fatal
        QCOMPARE(result.fileCount, static_cast<std::uint64_t>(0));
    }

    void telemetryEndsWithStructuredSummary()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        const std::filesystem::path root{tmp.path().toStdWString()};
        writeBytes(root / "a.bin", 100);
        writeBytes(root / "b.bin", 200);
        std::filesystem::create_directory(root / "sub");
        writeBytes(root / "sub" / "c.bin", 300);

        ScanEngine engine{};
        std::vector<std::string> telemetry{};
        std::mutex telemetryMutex{};
        usa::scanner::EngineCallbacks cb{};
        cb.onBatch = [&](std::vector<EngineNode>) {};
        cb.onTelemetry = [&](const std::string& json) {
            std::lock_guard<std::mutex> lock{telemetryMutex};
            telemetry.push_back(json);
        };
        const auto result = engine.run(root, cb, "t9");
        QCOMPARE(result.fileCount, static_cast<std::uint64_t>(3));
        QVERIFY(!telemetry.empty()); // at minimum the final summary always fires

        // The final summary carries the scan id and exact totals, and its
        // backend tag matches the backend the run actually used.
        const std::string& last = telemetry.back();
        const std::string backendStr =
            (result.backend == usa::scanner::BackendKind::DirectMft) ? "DirectMft"
                                                                     : "NativeDirectory";
        QVERIFY(last.find("\"scan_id\":\"t9\"") != std::string::npos);
        QVERIFY(last.find("\"files\":3") != std::string::npos);
        QVERIFY(last.find("\"backend\":\"" + backendStr + "\"") != std::string::npos);
        QVERIFY(last.find("ScanEngine/" + backendStr + "/final") != std::string::npos);
        QVERIFY(last.find("\"timestamp\":\"") != std::string::npos);
    }

    void fileTargetIsFatal()
    {
        // A file is not scannable: report explicitly instead of silently
        // finishing with zeros. Deterministic on every platform.
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        QFile f{tmp.filePath(QStringLiteral("plain.txt"))};
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(16, 'q'));
        f.close();

        ScanEngine engine{};
        bool sawError{false};
        bool sawFatal{false};
        usa::scanner::EngineCallbacks cb{};
        cb.onError = [&](const std::string&, bool fatal) {
            sawError = true;
            sawFatal = fatal;
        };
        const auto result =
            engine.run(std::filesystem::path{tmp.filePath(QStringLiteral("plain.txt")).toStdWString()},
                       cb);
        QVERIFY(sawError);
        QVERIFY(sawFatal);
        QCOMPARE(result.fileCount, static_cast<std::uint64_t>(0));
    }

    void topNRespected()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        const std::filesystem::path root{tmp.path().toStdWString()};
        writeBytes(root / "small.bin", 10);
        writeBytes(root / "big.bin", 500);
        writeBytes(root / "mid.bin", 100);

        ScanEngine engine{};
        engine.setTopN(2);
        std::vector<usa::scanner::AnalyticsSnapshot> snaps{};
        std::mutex snapsMutex{};
        usa::scanner::EngineCallbacks cb{};
        cb.onBatch = [&](std::vector<EngineNode>) {};
        cb.onAnalytics = [&](const usa::scanner::AnalyticsSnapshot& s) {
            std::lock_guard<std::mutex> lock{snapsMutex};
            snaps.push_back(s);
        };
        const auto result = engine.run(root, cb);
        QCOMPARE(result.fileCount, static_cast<std::uint64_t>(3));
        QVERIFY(!snaps.empty()); // exact finals always fire
        const auto& top = snaps.back().top;
        QCOMPARE(top.size(), static_cast<std::size_t>(2));
        QCOMPARE(top[0].bytes, static_cast<std::uint64_t>(500));
        QCOMPARE(top[1].bytes, static_cast<std::uint64_t>(100));
    }

    void emitsExactAnalyticsSnapshot()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        const std::filesystem::path root{tmp.path().toStdWString()};
        // Backdate with file-clock arithmetic (portable across OSes).
        const auto backdate = [&](const std::filesystem::path& p, int hours) {
            std::error_code ec{};
            const auto ft = std::filesystem::last_write_time(p, ec);
            QVERIFY(!ec);
            std::filesystem::last_write_time(p, ft - std::chrono::hours(hours), ec);
            QVERIFY(!ec);
        };
        writeBytes(root / "a.log", 100);
        backdate(root / "a.log", 48); // 2 days -> bucket [1,7)
        writeBytes(root / "b.LOG", 200);
        backdate(root / "b.LOG", 24 * 40); // 40 days -> bucket [30,180)
        writeBytes(root / "c.mp4", 300);
        backdate(root / "c.mp4", 24 * 400); // 400 days -> bucket [365,inf)
        writeBytes(root / "noext", 50); // now -> bucket [0,1)
        std::filesystem::create_directory(root / "sub");
        writeBytes(root / "sub" / "d.log", 400);
        backdate(root / "sub" / "d.log", 24 * 10); // 10 days -> [7,30)

        ScanEngine engine{};
        std::vector<usa::scanner::AnalyticsSnapshot> snaps{};
        std::mutex snapsMutex{};
        usa::scanner::EngineCallbacks cb{};
        cb.onBatch = [&](std::vector<EngineNode>) {};
        cb.onAnalytics = [&](const usa::scanner::AnalyticsSnapshot& s) {
            std::lock_guard<std::mutex> lock{snapsMutex};
            snaps.push_back(s);
        };
        const auto result = engine.run(root, cb);
        QCOMPARE(result.fileCount, static_cast<std::uint64_t>(5));
        QVERIFY(!snaps.empty());
        const auto& snap = snaps.back();

        // Extensions fold case; files without one are grouped honestly.
        std::map<std::string, usa::scanner::ExtensionStat> ext{};
        for (const auto& [name, st] : snap.extensions) {
            ext[name] = st;
        }
        QCOMPARE(ext.size(), static_cast<std::size_t>(3));
        QCOMPARE(ext.at(".log").bytes, static_cast<std::uint64_t>(700));
        QCOMPARE(ext.at(".log").files, static_cast<std::uint64_t>(3));
        QCOMPARE(ext.at(".mp4").bytes, static_cast<std::uint64_t>(300));
        QCOMPARE(ext.at("(no extension)").bytes, static_cast<std::uint64_t>(50));

        // Global top files, exact and ordered.
        QCOMPARE(snap.top.size(), static_cast<std::size_t>(5));
        const std::uint64_t wantTop[] = {400, 300, 200, 100, 50};
        for (std::size_t i{0}; i < 5; ++i) {
            QCOMPARE(snap.top[i].bytes, wantTop[i]);
        }

        // Default buckets {1,7,30,180,365}: [50,100,400,200,0,300].
        const std::uint64_t wantAge[] = {50, 100, 400, 200, 0, 300};
        QCOMPARE(snap.ageBytes.size(), static_cast<std::size_t>(6));
        for (std::size_t i{0}; i < 6; ++i) {
            QCOMPARE(snap.ageBytes[i], wantAge[i]);
        }

        // Owners cover every byte exactly (ids are backend-dependent).
        std::uint64_t ownerBytes{0};
        std::uint64_t ownerFiles{0};
        for (const auto& [id, st] : snap.owners) {
            QVERIFY(!id.empty());
            ownerBytes += st.bytes;
            ownerFiles += st.files;
        }
        QCOMPARE(ownerBytes, static_cast<std::uint64_t>(1050));
        QCOMPARE(ownerFiles, static_cast<std::uint64_t>(5));
    }
};

QTEST_MAIN(TestScanEngine)
#include "test_scanengine.moc"
