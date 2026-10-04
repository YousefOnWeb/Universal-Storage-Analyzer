#include <QtTest>

#include "../src/domain/FileTree.h"

using usa::domain::FileTree;

class TestFileTree : public QObject {
    Q_OBJECT
private slots:
    void addsNodesAndFindsThem()
    {
        FileTree tree{};
        const auto dir = tree.addNode(0, "docs", true, 0, 0, 0);
        const auto file = tree.addNode(dir, "a.txt", false, 100, 4096, 123);
        QVERIFY(dir != 0);
        QVERIFY(file != 0);
        const auto found = tree.find(file);
        QVERIFY(found.has_value());
        QCOMPARE(QString::fromStdString(found->name), QStringLiteral("a.txt"));
        QCOMPARE(found->logicalSize, static_cast<std::uint64_t>(100));
        QVERIFY(tree.nodeCount() >= 3);
    }

    void propagatesSizesToAncestors()
    {
        FileTree tree{};
        const auto dir = tree.addNode(0, "d", true, 0, 0, 0);
        tree.addNode(dir, "f.bin", false, 1000, 4096, 0);
        tree.propagateSizeUp(dir, 1000, 4096);
        const auto updated = tree.find(dir);
        QVERIFY(updated.has_value());
        QCOMPARE(updated->logicalSize, static_cast<std::uint64_t>(1000));
        const auto root = tree.find(tree.rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->logicalSize, static_cast<std::uint64_t>(1000));
    }

    void propagatesCountsToAncestors()
    {
        FileTree tree{};
        const auto dir = tree.addNode(0, "d", true, 0, 0, 0);
        tree.propagateCountUp(dir, false);
        const auto updated = tree.find(dir);
        QVERIFY(updated.has_value());
        QCOMPARE(updated->fileCount, static_cast<std::uint32_t>(1));
    }
};

QTEST_MAIN(TestFileTree)
#include "test_filetree.moc"
