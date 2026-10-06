#include <QtTest>

#include <QTemporaryDir>

#ifdef _WIN32
#include <QStorageInfo>
#endif

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

    // $STANDARD_INFORMATION at 48: 48 content bytes.
    putU32(buf, 48, 0x10);
    putU32(buf, 52, 72);
    buf[56] = 0; // resident
    buf[57] = 0; // unnamed
    putU32(buf, 64, 48); // content length
    putU16(buf, 68, 24); // content offset
    putU64(buf, 72, kTestFiletime); // creation
    putU64(buf, 80, kTestFiletime); // modification
    putU64(buf, 88, kTestFiletime); // MFT change
    putU64(buf, 96, kTestFiletime); // access
    putU32(buf, 104, 0x20); // archive attribute, not reparse

    // $FILE_NAME at 120: content 74 bytes (66 + 4 wide chars).
    putU32(buf, 120, 0x30);
    putU32(buf, 124, 98);
    buf[128] = 0; // resident
    buf[129] = 0; // unnamed
    putU32(buf, 136, 74); // content length
    putU16(buf, 140, 24); // content offset
    const std::size_t c = 120 + 24;
    putU64(buf, c + 0, static_cast<std::uint64_t>(5) | (static_cast<std::uint64_t>(5) << 48));
    putU64(buf, c + 24, kTestFiletime); // modification time
    putU64(buf, c + 48, 12345); // real size
    buf[c + 64] = 4; // name length
    buf[c + 65] = 1; // Win32 namespace
    buf[c + 66] = 't';
    buf[c + 68] = 'e';
    buf[c + 70] = 's';
    buf[c + 72] = 't';

    // Resident $DATA at 218 with 12 content bytes.
    putU32(buf, 218, 0x80);
    putU32(buf, 222, 36);
    buf[226] = 0;
    buf[227] = 0;
    putU32(buf, 234, 12);
    putU16(buf, 238, 24);
    for (int i{0}; i < 12; ++i) {
        buf[218 + 24 + static_cast<std::size_t>(i)] = static_cast<unsigned char>('a' + i);
    }
    putU32(buf, 254, 0xFFFFFFFFU); // end marker
    return buf;
}

} // namespace

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
        QCOMPARE(rec.physicalSize, static_cast<std::uint64_t>(12));
        QVERIFY(!rec.physicalIsClusterCount);
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
        bool sawNotepad{false};
        std::uint64_t mftSize{0};
        while (finder.findNext()) {
            const auto& e = finder.current();
            if (e.fullPath.filename() == "notepad.exe") {
                sawNotepad = true;
                mftSize = e.logicalSize;
            }
        }
        QVERIFY(sawNotepad);
        // Cross-validate against the portable view of the same file.
        std::error_code ec{};
        const auto directSize = std::filesystem::file_size(sysRoot / "notepad.exe", ec);
        QVERIFY(!ec);
        QCOMPARE(mftSize, static_cast<std::uint64_t>(directSize));

        // Directory backend agrees on the same entry.
        DirectoryFinder basic{};
        QVERIFY(basic.open(sysRoot));
        std::uint64_t enumSize{0};
        bool sawEnum{false};
        while (basic.findNext()) {
            if (basic.current().fullPath.filename() == "notepad.exe") {
                sawEnum = true;
                enumSize = basic.current().logicalSize;
            }
        }
        QVERIFY(sawEnum);
        QCOMPARE(mftSize, enumSize);
    }
#endif
};

QTEST_MAIN(TestMftFinder)
#include "test_mftfinder.moc"
