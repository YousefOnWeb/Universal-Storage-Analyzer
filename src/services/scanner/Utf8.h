#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// Infallible filesystem-name encoding (Qt-free, portable).
//
// std::filesystem::path converters THROW on Windows when a name holds an
// unpaired surrogate (legal in NTFS, seen on real system volumes). A throw
// inside an engine worker thread escapes through std::thread and kills the
// whole process via std::terminate — observed as fail-fast 0xc0000409 at
// scan start. These helpers never throw: lone units become U+FFFD.
namespace usa::scanner::detail {

inline void appendUtf8(std::string& out, char32_t codePoint)
{
    if (codePoint < 0x80U) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else if (codePoint < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
}

inline std::string utf16ToUtf8(const std::wstring& wide)
{
    std::string out{};
    out.reserve(wide.size());
    for (std::size_t i{0}; i < wide.size();) {
        const std::uint32_t unit = static_cast<std::uint32_t>(wide[i]);
        if (unit >= 0xD800U && unit <= 0xDBFFU && i + 1U < wide.size()) {
            const std::uint32_t low = static_cast<std::uint32_t>(wide[i + 1]);
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                appendUtf8(out, 0x10000U + ((unit - 0xD800U) << 10U) + (low - 0xDC00U));
                i += 2U;
                continue;
            }
        }
        if (unit >= 0xD800U && unit <= 0xDFFFU) {
            appendUtf8(out, 0xFFFDU); // lone surrogate
        } else {
            appendUtf8(out, static_cast<char32_t>(unit));
        }
        ++i;
    }
    return out;
}

// Leaf filename (leafOnly) or full path, as UTF-8, never throwing.
inline std::string pathToUtf8(const std::filesystem::path& path, bool leafOnly)
{
#ifdef _WIN32
    const std::filesystem::path base =
        (leafOnly && !path.filename().empty()) ? path.filename() : path;
    return utf16ToUtf8(base.native());
#else
    // POSIX names are opaque bytes: no conversion, no failure mode.
    const std::filesystem::path base =
        (leafOnly && !path.filename().empty()) ? path.filename() : path;
    const auto raw = base.native();
    return {raw.data(), raw.size()};
#endif
}

} // namespace usa::scanner::detail
