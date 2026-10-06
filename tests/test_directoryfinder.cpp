#include <QtTest>

#include <QTemporaryDir>

#include <cstdint>
#include <fstream>
#include <map>
#include <string>

#include "../src/services/scanner/DirectoryFinder.h"

using usa::scanner::DirectoryFinder;
using usa::scanner::DirEntry;

namespace {

void writeBytes(const std::filesystem::path& p, std::size_t n, char fill)
{
    std::ofstream f{p, std::ios::binary};
    const std::string data(n, fill);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::map<std::u8string, const DirEntry*> byName(const std::vector<DirEntry>& entries)
{
    std::map<std::u8string, const DirEntry*> out{};
    for (const auto& e : entries) {
        out[e.fullPath.filename().u8string()] = &e;
    }
    return out;
}

} // namespace

class TestDirectoryFinder : public QObject {
    Q_OBJECT
private slots:
    void enumeratesKnownTree()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        const std::filesystem::path root{tmp.path().toStdWString()};
        writeBytes(root / "a.txt", 100, 'x');
        writeBytes(root / "empty.bin", 0, 'x');
        std::filesystem::create_directory(root / "sub");
        writeBytes(root / "sub" / "b.bin", 5000, 'y');

        DirectoryFinder finder{};
        QVERIFY(finder.open(root));
        std::vector<DirEntry> entries{};
        while (finder.findNext()) {
            entries.push_back(finder.current());
        }
        finder.close();
        QCOMPARE(entries.size(), static_cast<std::size_t>(3));

        const auto map = byName(entries);
        QVERIFY(map.contains(u8"a.txt"));
        QVERIFY(map.contains(u8"empty.bin"));
        QVERIFY(map.contains(u8"sub"));
        QCOMPARE(map.at(u8"a.txt")->logicalSize, static_cast<std::uint64_t>(100));
        QCOMPARE(map.at(u8"empty.bin")->logicalSize, static_cast<std::uint64_t>(0));
        QVERIFY(map.at(u8"sub")->isDirectory);
        QVERIFY(!map.at(u8"a.txt")->isDirectory);
        QVERIFY(map.at(u8"a.txt")->lastWriteTs != 0);
        QVERIFY(map.at(u8"a.txt")->physicalSize >= map.at(u8"a.txt")->logicalSize);

        // Subdirectory enumerates independently.
        QVERIFY(finder.open(root / "sub"));
        std::vector<DirEntry> subEntries{};
        while (finder.findNext()) {
            subEntries.push_back(finder.current());
        }
        QCOMPARE(subEntries.size(), static_cast<std::size_t>(1));
        QCOMPARE(subEntries.front().logicalSize, static_cast<std::uint64_t>(5000));
    }

    void rejectsMissingPath()
    {
        DirectoryFinder finder{};
        QVERIFY(!finder.open(std::filesystem::path{"Z:/definitely-not-here-usa-test"}));
        QVERIFY(!finder.findNext());
    }

    void marksSymlinkAsReparsePoint()
    {
        QTemporaryDir tmp{};
        QVERIFY(tmp.isValid());
        const std::filesystem::path root{tmp.path().toStdWString()};
        writeBytes(root / "real.txt", 10, 'z');
        std::error_code ec{};
        std::filesystem::create_directory_symlink(root, root / "linkdir", ec);
        if (ec) {
            QSKIP("Cannot create symlinks in this environment (often needs "
                  "Developer Mode / privilege on Windows).");
        }
        DirectoryFinder finder{};
        QVERIFY(finder.open(root));
        bool sawLink{false};
        while (finder.findNext()) {
            if (finder.current().fullPath.filename() == "linkdir") {
                sawLink = true;
                QVERIFY(finder.current().isReparsePoint);
                QVERIFY(!finder.current().isDirectory); // never descended into
            }
        }
        QVERIFY(sawLink);
    }
};

QTEST_MAIN(TestDirectoryFinder)
#include "test_directoryfinder.moc"
