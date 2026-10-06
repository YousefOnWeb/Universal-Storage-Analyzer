#include <QtTest>

#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

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
        // Portable backend on every OS (direct volume reading is opt-in).
        QCOMPARE(result.backend, usa::scanner::BackendKind::NativeDirectory);
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
        usa::scanner::EngineCallbacks cb{};
        cb.onError = [&](const std::string&) { sawError = true; };
        const auto result = engine.run(std::filesystem::path{"Z:/definitely-not-here-usa-test"}, cb);
        QVERIFY(sawError);
        QCOMPARE(result.fileCount, static_cast<std::uint64_t>(0));
    }
};

QTEST_MAIN(TestScanEngine)
#include "test_scanengine.moc"
