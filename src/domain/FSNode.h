#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace usa::domain {

// Core data model — Sprint 1 focus (see docs/specs/001-sprint1-foundation.md §2.1).
// Aggressively packed, Qt-free: the domain layer must not know Qt Widgets exists.
struct FSNode {
    std::uint32_t id{0};
    std::uint32_t parentId{0};
    std::string name{};
    std::uint64_t logicalSize{0};
    std::uint64_t allocatedSize{0};
    std::uint32_t fileCount{0};
    std::uint32_t folderCount{0};
    std::uint64_t lastModifiedTs{0};
    bool isDirectory{false};
    bool isCompressed{false};
    std::vector<std::uint32_t> children{};
};

} // namespace usa::domain
