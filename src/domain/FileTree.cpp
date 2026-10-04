#include "FileTree.h"

namespace usa::domain {

FileTree::FileTree()
{
    FSNode root{};
    root.id = 0;
    root.parentId = 0;
    root.isDirectory = true;
    nodes_.emplace(0, std::move(root));
}

void FileTree::clear()
{
    std::lock_guard<std::mutex> lock{mutex_};
    nodes_.clear();
    FSNode root{};
    root.id = 0;
    root.parentId = 0;
    root.isDirectory = true;
    nodes_.emplace(0, std::move(root));
    rootId_ = 0;
    nextId_ = 1;
}

std::uint32_t FileTree::addNode(std::uint32_t parentId, std::string name, bool isDirectory,
                                std::uint64_t logicalSize, std::uint64_t allocatedSize,
                                std::uint64_t lastModifiedTs, bool isCompressed)
{
    std::lock_guard<std::mutex> lock{mutex_};
    const std::uint32_t id{nextId_++};

    FSNode node{};
    node.id = id;
    node.parentId = parentId;
    node.name = std::move(name);
    node.logicalSize = logicalSize;
    node.allocatedSize = allocatedSize;
    node.lastModifiedTs = lastModifiedTs;
    node.isDirectory = isDirectory;
    node.isCompressed = isCompressed;
    nodes_.emplace(id, node);

    if (const auto parentIt = nodes_.find(parentId); parentIt != nodes_.end()) {
        parentIt->second.children.push_back(id);
    } else if (const auto rootIt = nodes_.find(rootId_); rootIt != nodes_.end()) {
        rootIt->second.children.push_back(id);
        nodes_.at(id).parentId = rootId_;
    }
    return id;
}

void FileTree::propagateSizeUp(std::uint32_t nodeId, std::uint64_t logicalDelta,
                               std::uint64_t allocatedDelta)
{
    std::lock_guard<std::mutex> lock{mutex_};
    std::uint32_t current{nodeId};
    for (int depth{0}; depth < 1'000'000; ++depth) {
        const auto it = nodes_.find(current);
        if (it == nodes_.end()) {
            return;
        }
        it->second.logicalSize += logicalDelta;
        it->second.allocatedSize += allocatedDelta;
        if (current == it->second.parentId) {
            return; // reached root (parent of root is itself)
        }
        current = it->second.parentId;
    }
}

void FileTree::propagateCountUp(std::uint32_t nodeId, bool childIsDirectory)
{
    std::lock_guard<std::mutex> lock{mutex_};
    std::uint32_t current{nodeId};
    for (int depth{0}; depth < 1'000'000; ++depth) {
        const auto it = nodes_.find(current);
        if (it == nodes_.end()) {
            return;
        }
        if (childIsDirectory) {
            ++it->second.folderCount;
        } else {
            ++it->second.fileCount;
        }
        if (current == it->second.parentId) {
            return;
        }
        current = it->second.parentId;
    }
}

std::optional<FSNode> FileTree::find(std::uint32_t id) const
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (const auto it = nodes_.find(id); it != nodes_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::uint32_t FileTree::rootId() const
{
    return rootId_;
}

std::size_t FileTree::nodeCount() const
{
    std::lock_guard<std::mutex> lock{mutex_};
    return nodes_.size();
}

} // namespace usa::domain
