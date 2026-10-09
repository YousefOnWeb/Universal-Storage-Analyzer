#include <QtTest>

#include <QTemporaryDir>

#ifdef _WIN32
#include <QStorageInfo>
#endif

#include <algorithm>
#include <filesystem>

#include "../src/services/scanner/DirectoryFinder.h"
#include "../src/services/scanner/MftFinder.h"
#include "../src/services/scanner/MftRecord.h"

using usa::scanner::DirectoryFinder;
using usa::scanner::MftFinder;

namespace {

constexpr std::uint64_t kTestFiletime{133000000000000000ULL};

void putU16(std::vector<unsigned char>& buf, std::size_t off, std::uint16_t v)
{
    buf[off] = static_cast<unsigned char>(v & 0xFF);
    buf[off + 1] = static_cast<unsigned char>((v >> 8) & 0xFF);
}

void putU32(std::vector<unsigned char>& buf, std::size_t off, std::uint32_t v)
{
    for (int i{0}; i < 4; ++i) {
        buf[off + static_cast<std::size_t>(i)] = static_cast<unsigned char>((v >> (i * 8)) & 0xFF);
    }
}

void putU64(std::vector<unsigned char>& buf, std::size_t off, std::uint64_t v)
{
    for (int i{0}; i < 8; ++i) {
        buf[off + static_cast<std::size_t>(i)] = static_cast<unsigned char>((v >> (i * 8)) & 0xFF);
    }
}

// Builds a valid 1024-byte record: in-use file "test", parent 5, logical
// size 12345, 12 bytes of resident data, known modification time.
std::vector<unsigned char> makeFileRecord()
{
    std::vector<unsigned char> buf(1024, 0);
    buf[0] = 'F';
    buf[1] = 'I';
    buf[2] = 'L';
    buf[3] = 'E';
    putU16(buf, 4, 42); // USA offset
    putU16(buf, 6, 3); // USA count: USN + 2 sectors
    putU16(buf, 16, 7); // sequence
    putU16(buf, 20, 48); // first attribute offset
    putU16(buf, 22, 0x01); // in use, plain file
    putU16(buf, 42, 0x1234); // USN
    putU16(buf, 44, 0xAAAA); // fixup sector 0
    putU16(buf, 46, 0xBBBB); // fixup sector 1
    putU16(buf, 510, 0x1234); // sector tails carry the USN
    putU16(buf, 1022, 0x1234);

    // $STANDARD_INFORMATION at 48: 56 content bytes (owner id at +48).
    putU32(buf, 48, 0x10);
    putU32(buf, 52, 80);
    buf[56] = 0; // resident
    buf[57] = 0; // unnamed
    putU32(buf, 64, 56); // content length
    putU16(buf, 68, 24); // content offset
    putU64(buf, 72, kTestFiletime); // creation
    putU64(buf, 80, kTestFiletime); // modification
    putU64(buf, 88, kTestFiletime); // MFT change
    putU64(buf, 96, kTestFiletime); // access
    putU32(buf, 104, 0x20); // archive attribute, not reparse
    putU32(buf, 120, 1001); // owner id

    // $FILE_NAME at 128: content 74 bytes (66 + 4 wide chars).
    putU32(buf, 128, 0x30);
    putU32(buf, 132, 98);
    buf[136] = 0; // resident
    buf[137] = 0; // unnamed
    putU32(buf, 144, 74); // content length
    putU16(buf, 148, 24); // content offset
    const std::size_t c = 128 + 24;
    putU64(buf, c + 0, static_cast<std::uint64_t>(5) | (static_cast<std::uint64_t>(5) << 48));
    putU64(buf, c + 24, kTestFiletime); // modification time
    putU64(buf, c + 40, 16384); // allocated size (MS-FSCC 2.4: +40)
    putU64(buf, c + 48, 12345); // real size
    putU32(buf, c + 56, 0x20); // flags: archive, not dir/reparse
    buf[c + 64] = 4; // name length
    buf[c + 65] = 1; // Win32 namespace
    buf[c + 66] = 't';
    buf[c + 68] = 'e';
    buf[c + 70] = 's';
    buf[c + 72] = 't';

    // Resident $DATA at 226 with 12 content bytes.
    putU32(buf, 226, 0x80);
    putU32(buf, 230, 36);
    buf[234] = 0;
    buf[235] = 0;
    putU32(buf, 242, 12);
    putU16(buf, 246, 24);
    for (int i{0}; i < 12; ++i) {
        buf[226 + 24 + static_cast<std::size_t>(i)] = static_cast<unsigned char>('a' + i);
    }
    putU32(buf, 262, 0xFFFFFFFFU); // end marker
    return buf;
}

} // namespace

// Rewrites the fixture's resident $DATA attribute (at offset 226) as a
// non-resident one with runs [3 clusters at LCN 100][2 sparse clusters].
// Compressed layouts reserve the 8-byte compressed-size field, shifting
// the run list by 8.
void patchNonResidentData(std::vector<unsigned char>& buf, bool compressed, std::uint64_t compSize)
{
    putU32(buf, 226, 0x80);
    buf[234] = 1; // non-resident
    buf[235] = 0; // unnamed
    putU16(buf, 238, compressed ? 1 : 0); // attribute flags
    const std::uint16_t runOff = compressed ? 72 : 64;
    putU16(buf, 258, runOff);
    putU64(buf, 266, 12288); // allocated size
    const std::size_t r = 226 + runOff;
    if (compressed) {
        putU64(buf, 290, compSize);
    }
    buf[r + 0] = 0x11;
    buf[r + 1] = 3;
    buf[r + 2] = 100; // run 1: data
    buf[r + 3] = 0x11;
    buf[r + 4] = 2;
    buf[r + 5] = 0xFF; // run 2: sparse (LCN -1)
    buf[r + 6] = 0x00; // terminator
    const std::uint32_t len = static_cast<std::uint32_t>(runOff + 7);
    putU32(buf, 230, len);
    putU32(buf, 226 + len, 0xFFFFFFFFU); // end marker
}

class TestMftFinder : public QObject {
    Q_OBJECT
private slots:
    void parsesSyntheticFileRecord()
    {
        const auto buf = makeFileRecord();
        usa::scanner::detail::MftRecord rec{};
        QVERIFY(usa::scanner::detail::parseMftRecord(buf.data(), buf.size(), 42, rec));
        QCOMPARE(rec.number, static_cast<std::uint32_t>(42));
        QCOMPARE(rec.sequence, static_cast<std::uint16_t>(7));
        QVERIFY(rec.inUse);
        QVERIFY(!rec.isDirectory);
        QVERIFY(rec.name == L"test");
        QCOMPARE(rec.parentNumber, static_cast<std::uint32_t>(5));
        QCOMPARE(rec.parentSequence, static_cast<std::uint16_t>(5));
        QCOMPARE(rec.logicalSize, static_cast<std::uint64_t>(12345));
        QCOMPARE(rec.residentBytes, static_cast<std::uint64_t>(12));
        QVERIFY(rec.hasOwner);
        QCOMPARE(rec.ownerId, static_cast<std::uint32_t>(1001));
        QCOMPARE(rec.nonResidentClusters, static_cast<std::uint64_t>(0));
        QVERIFY(rec.usesClusterMath);
        QCOMPARE(rec.lastWriteTs,
                 usa::scanner::detail::filetimeToUnix(kTestFiletime));
    }

    void rejectsCorruptRecords()
    {
        auto torn = makeFileRecord();
        putU16(torn, 510, 0x9999); // sector tail disagrees with USN
        usa::scanner::detail::MftRecord rec{};
        QVERIFY(!usa::scanner::detail::parseMftRecord(torn.data(), torn.size(), 42, rec));

        auto badMagic = makeFileRecord();
        badMagic[0] = 'X';
        QVERIFY(!usa::scanner::detail::parseMftRecord(badMagic.data(), badMagic.size(), 42, rec));

        auto notInUse = makeFileRecord();
        putU16(notInUse, 22, 0x00);
        QVERIFY(
            !usa::scanner::detail::parseMftRecord(notInUse.data(), notInUse.size(), 42, rec));
    }

    void decodesNonResidentRunsSkippingSparse()
    {
        // Rewrites the fixture's resident $DATA (at 226) as non-resident:
        // 3 clusters at LCN 100, then 2 sparse clusters.
        auto buf = makeFileRecord();
        patchNonResidentData(buf, false, 0);
        usa::scanner::detail::MftRecord rec{};
        QVERIFY(usa::scanner::detail::parseMftRecord(buf.data(), buf.size(), 42, rec));
        QVERIFY(rec.usesClusterMath);
        QCOMPARE(rec.nonResidentClusters, static_cast<std::uint64_t>(3));
        QCOMPARE(rec.residentBytes, static_cast<std::uint64_t>(0));
        QCOMPARE(rec.logicalSize, static_cast<std::uint64_t>(12345));
    }

    void prefersCompressedSizeForCompressedData()
    {
        auto buf = makeFileRecord();
        patchNonResidentData(buf, true, 500);
        usa::scanner::detail::MftRecord rec{};
        QVERIFY(usa::scanner::detail::parseMftRecord(buf.data(), buf.size(), 42, rec));
        QVERIFY(rec.usesClusterMath);
        QCOMPARE(rec.residentBytes, static_cast<std::uint64_t>(500));
        QCOMPARE(rec.nonResidentClusters, static_cast<std::uint64_t>(0));
    }

    void decodesSparseRunsWithZeroLcnField()
    {
        // NTFS encodes sparseness as a run with a zero-length LCN field:
        // [3 clusters at LCN 100][2 sparse clusters][terminator].
        const unsigned char runs[] = {0x11, 0x03, 0x64, 0x01, 0x02, 0x00};
        std::vector<usa::scanner::detail::DataRun> out{};
        QVERIFY(usa::scanner::detail::parseDataRuns(runs, sizeof(runs), out));
        QCOMPARE(out.size(), static_cast<std::size_t>(2));
        QCOMPARE(out[0].lengthClusters, static_cast<std::uint64_t>(3));
        QCOMPARE(out[0].startLcn, static_cast<std::int64_t>(100));
        QVERIFY(!out[0].sparse);
        QCOMPARE(out[1].lengthClusters, static_cast<std::uint64_t>(2));
        QVERIFY(out[1].sparse);
    }

    void usesFileNameAllocSizeForMultiExtentFallback()
    {
        // A record whose $DATA lives in an extension record carries an
        // $ATTRIBUTE_LIST and no $DATA: physicalSize must fall back to the
        // $FILE_NAME allocated size (offset +40), not the flags at +56.
        auto buf = makeFileRecord();
        // Insert a minimal $ATTRIBUTE_LIST before the $FILE_NAME at 128.
        // Shift the tail up by 8 bytes; the fixture has slack after 262.
        std::copy_backward(buf.begin() + 128, buf.begin() + 270, buf.begin() + 278);
        putU32(buf, 128, 0x20); // $ATTRIBUTE_LIST
        putU32(buf, 132, 8); // header-only length
        // Erase the resident $DATA (now at 234) back to an end marker.
        putU32(buf, 234, 0xFFFFFFFFU);
        usa::scanner::detail::MftRecord rec{};
        QVERIFY(usa::scanner::detail::parseMftRecord(buf.data(), buf.size(), 42, rec));
        QVERIFY(!rec.usesClusterMath);
        QCOMPARE(rec.physicalSize, static_cast<std::uint64_t>(16384));
        QCOMPARE(rec.logicalSize, static_cast<std::uint64_t>(12345));
        QVERIFY(!rec.isDirectory);
        QVERIFY(!rec.isReparsePoint);
    }
    void reportsAvailabilityConsistently()
    {
        // Empty paths are never eligible, on any OS.
        QVERIFY(!usa::scanner::isDirectScanAvailable(std::filesystem::path{}));
#ifdef _WIN32
        // A drive letter with no mounted volume is never eligible. Hunt
        // one at runtime instead of assuming a letter is free (this
        // machine, for example, has a real Z: drive).
        QString bogus{};
        for (char letter = 'Q'; letter <= 'Z'; ++letter) {
            const QString candidate =
                QStringLiteral("%1:/definitely-not-here-usa-test").arg(QChar(letter));
            if (!QStorageInfo(candidate).isValid()) {
                bogus = candidate;
                break;
            }
        }
        if (bogus.isEmpty()) {
            QSKIP("No free drive letter Q-Z to test unmounted volumes.");
        }
        QVERIFY(!usa::scanner::isDirectScanAvailable(
            std::filesystem::path{bogus.toStdWString()}));
#endif
    }

    void stubBackendIsInert()
    {
        MftFinder finder{};
        QVERIFY(!finder.volumeLoaded());
        QCOMPARE(finder.recordCount(), static_cast<std::size_t>(0));
        QVERIFY(!finder.findNext());
#ifndef _WIN32
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        QVERIFY(!finder.open(std::filesystem::path{tmp.path().toStdString()}));
#endif
    }

#ifdef _WIN32
    void readsLiveVolumeWhenElevated()
    {
        const std::filesystem::path sysRoot{L"C:\\Windows\\System32"};
        if (!usa::scanner::isDirectScanAvailable(sysRoot)) {
            QSKIP("Requires elevation on an NTFS volume; falling back is "
                  "covered by the directory backend tests.");
        }
        MftFinder finder{};
        const bool loaded = finder.loadVolume(sysRoot);
        if (!loaded) {
            qInfo() << "loadVolume failed:" << QString::fromStdString(finder.lastError());
        }
        QVERIFY(loaded);
        QVERIFY(finder.volumeLoaded());
        QVERIFY(finder.recordCount() > 1000);

        QVERIFY(finder.open(sysRoot));
        bool sawAnyFile{false};
        std::uint64_t mftSize{0};
        std::filesystem::path foundFile{};

        while (finder.findNext()) {
            const auto& e = finder.current();
            if (!e.isDirectory && !e.isReparsePoint && e.logicalSize > 0) {
                sawAnyFile = true;
                mftSize = e.logicalSize;
                foundFile = e.fullPath;
                break;
            }
        }
        QVERIFY(sawAnyFile);

        // Cross-validate against the portable view of the same file.
        std::error_code ec{};
        const auto directSize = std::filesystem::file_size(foundFile, ec);
        QVERIFY(!ec);
        QCOMPARE(mftSize, static_cast<std::uint64_t>(directSize));

        // Directory backend agrees on the same entry.
        DirectoryFinder basic{};
        QVERIFY(basic.open(sysRoot));
        std::uint64_t enumSize{0};
        bool sawEnum{false};
        while (basic.findNext()) {
            if (basic.current().fullPath.filename() == foundFile.filename()) {
                sawEnum = true;
                enumSize = basic.current().logicalSize;
                break;
            }
        }
        QVERIFY(sawEnum);
        QCOMPARE(mftSize, enumSize);
    }
#endif
};

QTEST_MAIN(TestMftFinder)
#include "test_mftfinder.moc"
