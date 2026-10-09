#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace usa::scanner {

// Incremental per-scan analytics, fed from the single traversal pass.
// Files only (directories aggregate, they do not consume): per file the
// engine calls recordFile() once, O(1) amortized, memory bounded by the
// caps below. Snapshots are absolute replace-semantics values taken under
// the internal lock; callers store them verbatim, like dir summaries.
struct ExtensionStat {
    std::uint64_t bytes{0};
    std::uint64_t files{0};
};

struct TopEntry {
    std::uint64_t bytes{0};
    std::uint32_t id{0}; // engine node id: panes select it in the tree
    std::string pathUtf8{};
    std::string nameUtf8{};
};

struct OwnerStat {
    std::uint64_t bytes{0};
    std::uint64_t files{0};
};

struct AnalyticsSnapshot {
    // Extensions and owners sorted by bytes desc; top files desc.
    std::vector<std::pair<std::string, ExtensionStat>> extensions{};
    std::vector<TopEntry> top{};
    // Edges ride along so panes label buckets without a second channel.
    std::vector<std::uint64_t> ageEdges{};
    // Parallel to ageEdges.size()+1: bucket i covers [edge[i-1], edge[i]).
    std::vector<std::uint64_t> ageBytes{};
    std::vector<std::uint64_t> ageFiles{};
    std::vector<std::pair<std::string, OwnerStat>> owners{};
    // Running totals for share-% denominators.
    std::uint64_t totalBytes{0};
    std::uint64_t totalFiles{0};
};

class Analytics {
public:
    static constexpr std::size_t kMaxExtensions{4096};
    static constexpr const char* kNoExtension{"(no extension)"};
    static constexpr const char* kOtherExtension{"(other)"};
    static constexpr const char* kUnknownOwner{"(unknown)"};
    static constexpr std::size_t kMinTopN{1};
    static constexpr std::size_t kMaxTopN{65536};

    // Extension of a UTF-8 file name, ASCII-lowercased with the dot:
    // "clip.MP4" -> ".mp4", "noext"/".bashrc"/"a." -> "(no extension)".
    // Byte-wise (never splits a multibyte unit) and allocation-free on
    // miss paths. Pure and static for unit testing.
    static std::string extensionOf(const std::string& nameUtf8)
    {
        const std::size_t dot = nameUtf8.find_last_of('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= nameUtf8.size()) {
            return kNoExtension;
        }
        std::string ext{nameUtf8.substr(dot)};
        for (char& c : ext) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return ext;
    }

    Analytics()
    {
        ageBytes_.assign(ageEdges_.size() + 1, 0);
        ageFiles_.assign(ageEdges_.size() + 1, 0);
        heap_.reserve(topN_);
    }

    // Ascending day edges, e.g. {1,7,30,180,365} -> 6 buckets. Empty or
    // unsorted input falls back to the default. Pre-run configuration.
    void setAgeBuckets(std::vector<std::uint64_t> days)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (!days.empty() && std::is_sorted(days.begin(), days.end())) {
            ageEdges_ = std::move(days);
            ageBytes_.assign(ageEdges_.size() + 1, 0);
            ageFiles_.assign(ageEdges_.size() + 1, 0);
        }
    }

    // Largest-files heap capacity, clamped. Pre-run configuration.
    void setTopN(std::size_t n)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        topN_ = std::clamp(n, kMinTopN, kMaxTopN);
        heap_.reserve(topN_);
        while (heap_.size() > topN_) {
            std::pop_heap(heap_.begin(), heap_.end(), &heavier);
            heap_.pop_back();
        }
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        ext_.clear();
        owners_.clear();
        heap_.clear();
        totalBytes_ = 0;
        totalFiles_ = 0;
        ageBytes_.assign(ageEdges_.size() + 1, 0);
        ageFiles_.assign(ageEdges_.size() + 1, 0);
    }

    // One call per file node, from any worker thread. ageDays is ignored
    // when ageKnown is false (unknown mtimes are skipped, never bucketed
    // by guess). Empty ownerId aggregates into "(unknown)".
    void recordFile(std::uint32_t id, const std::string& nameUtf8, const std::string& pathUtf8,
                    std::uint64_t bytes, std::uint64_t ageDays, bool ageKnown,
                    const std::string& ownerId)
    {
        const std::string ext = extensionOf(nameUtf8);
        std::lock_guard<std::mutex> lock{mutex_};
        auto& es = ext_[ext];
        if (ext_.size() > kMaxExtensions && es.files == 0 && es.bytes == 0) {
            // First touch of a new extension past the cap: fold into
            // "(other)" instead of growing without bound.
            ext_.erase(ext);
            auto& other = ext_[kOtherExtension];
            other.bytes += bytes;
            ++other.files;
        } else {
            es.bytes += bytes;
            ++es.files;
        }
        auto& os = owners_[ownerId.empty() ? kUnknownOwner : ownerId];
        os.bytes += bytes;
        ++os.files;
        totalBytes_ += bytes;
        ++totalFiles_;
        if (ageKnown && !ageBytes_.empty()) {
            const std::size_t b = bucketFor(ageDays);
            ageBytes_[b] += bytes;
            ++ageFiles_[b];
        }
        if (heap_.size() < topN_) {
            heap_.push_back(TopEntry{bytes, id, pathUtf8, nameUtf8});
            std::push_heap(heap_.begin(), heap_.end(), &heavier);
        } else if (!heap_.empty() && bytes > heap_.front().bytes) {
            std::pop_heap(heap_.begin(), heap_.end(), &heavier);
            heap_.back() = TopEntry{bytes, id, pathUtf8, nameUtf8};
            std::push_heap(heap_.begin(), heap_.end(), &heavier);
        }
    }

    AnalyticsSnapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        AnalyticsSnapshot out{};
        out.extensions.reserve(ext_.size());
        for (const auto& [ext, st] : ext_) {
            out.extensions.emplace_back(ext, st);
        }
        std::sort(out.extensions.begin(), out.extensions.end(),
                  [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
        out.top = heap_;
        std::sort(out.top.begin(), out.top.end(),
                  [](const TopEntry& a, const TopEntry& b) { return a.bytes > b.bytes; });
        out.ageEdges = ageEdges_;
        out.ageBytes = ageBytes_;
        out.ageFiles = ageFiles_;
        out.totalBytes = totalBytes_;
        out.totalFiles = totalFiles_;
        out.owners.reserve(owners_.size());
        for (const auto& [owner, st] : owners_) {
            out.owners.emplace_back(owner, st);
        }
        std::sort(out.owners.begin(), out.owners.end(),
                  [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
        return out;
    }

private:
    // Min-heap ordering: the smallest sits on top for O(1) eviction tests.
    static bool heavier(const TopEntry& a, const TopEntry& b) { return a.bytes > b.bytes; }

    std::size_t bucketFor(std::uint64_t ageDays) const
    {
        for (std::size_t i{0}; i < ageEdges_.size(); ++i) {
            if (ageDays < ageEdges_[i]) {
                return i;
            }
        }
        return ageEdges_.size();
    }

    mutable std::mutex mutex_{};
    std::vector<std::uint64_t> ageEdges_{1, 7, 30, 180, 365};
    std::size_t topN_{1000};
    std::uint64_t totalBytes_{0};
    std::uint64_t totalFiles_{0};
    std::unordered_map<std::string, ExtensionStat> ext_{};
    std::unordered_map<std::string, OwnerStat> owners_{};
    std::vector<std::uint64_t> ageBytes_{};
    std::vector<std::uint64_t> ageFiles_{};
    std::vector<TopEntry> heap_{}; // min-heap by bytes while recording
};

} // namespace usa::scanner
