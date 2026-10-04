#pragma once

#include "FSNode.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace usa::domain {

// Thread-safe in-memory n-ary tree backing the scan.
// The scanner worker owns the single mutating instance; the UI mirrors it
// through queued signal batches (never by sharing this object across threads).
class FileTree {
public:
    FileTree();

    FileTree(const FileTree&) = delete;
    FileTree& operator=(const FileTree&) = delete;

    void clear();

    // Adds a node under parentId (0 = root). Returns the assigned id.
    std::uint32_t addNode(std::uint32_t parentId, std::string name, bool isDirectory,
                          std::uint64_t logicalSize, std::uint64_t allocatedSize,
                          std::uint64_t lastModifiedTs, bool isCompressed = false);

    // Adds delta sizes to nodeId and every ancestor up to the root.
    void propagateSizeUp(std::uint32_t nodeId, std::uint64_t logicalDelta,
                         std::uint64_t allocatedDelta);

    // Increments file/folder counters on nodeId and every ancestor.
    void propagateCountUp(std::uint32_t nodeId, bool childIsDirectory);

    std::optional<FSNode> find(std::uint32_t id) const;
    std::uint32_t rootId() const;
    std::size_t nodeCount() const;

private:
    mutable std::mutex mutex_{};
    std::unordered_map<std::uint32_t, FSNode> nodes_{};
    std::uint32_t rootId_{0};
    std::uint32_t nextId_{1};
};

} // namespace usa::domain
