#include "OwnerResolver.h"

#ifdef _WIN32
#include <windows.h>

#include <sddl.h>
#else
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cstdlib>

QString OwnerResolver::displayName(const std::string& ownerId)
{
    if (ownerId.empty() || ownerId == "(unknown)") {
        return QStringLiteral("(unknown)");
    }
    std::lock_guard<std::mutex> lock{mutex_};
    const auto cached = cache_.find(ownerId);
    if (cached != cache_.end()) {
        return cached->second;
    }
    QString name = resolve(ownerId);
    if (name.isEmpty()) {
        name = QString::fromStdString(ownerId); // unresolvable: raw id, never blank
    }
    cache_.emplace(ownerId, name);
    return name;
}

QString OwnerResolver::resolve(const std::string& ownerId)
{
#ifdef _WIN32
    // SID string -> account name. Handles domain accounts when joined;
    // workgroup/unknown SIDs fall back to the raw string at the caller.
    PSID sid{nullptr};
    if (ConvertStringSidToSidA(ownerId.c_str(), &sid) == 0 || sid == nullptr) {
        return {};
    }
    struct SidGuard {
        PSID s{nullptr};
        ~SidGuard()
        {
            if (s != nullptr) {
                LocalFree(s);
            }
        }
    } guard{sid};

    DWORD nameLen{0};
    DWORD domainLen{0};
    SID_NAME_USE use{};
    LookupAccountSidW(nullptr, sid, nullptr, &nameLen, nullptr, &domainLen, &use);
    if (nameLen == 0) {
        return {};
    }
    std::wstring name(nameLen, L'\0');
    std::wstring domain(domainLen, L'\0');
    if (LookupAccountSidW(nullptr, sid, name.data(), &nameLen, domain.data(), &domainLen,
                           &use)
        == 0) {
        return {};
    }
    // API writes the NUL into the size; drop it before comparing.
    while (!name.empty() && name.back() == L'\0') {
        name.pop_back();
    }
    while (!domain.empty() && domain.back() == L'\0') {
        domain.pop_back();
    }
    QString result = QString::fromStdWString(name);
    if (!domain.empty()) {
        result = QString::fromStdWString(domain) + QStringLiteral("\\") + result;
    }
    return result;
#else
    // Numeric uid -> passwd name. Non-numeric ids have no POSIX meaning.
    char* end{nullptr};
    const unsigned long uid = std::strtoul(ownerId.c_str(), &end, 10);
    if (end == ownerId.c_str() || *end != '\0') {
        return {};
    }
    struct passwd pwd {};
    struct passwd* found{nullptr};
    // Sized for typical entries; getpwuid_r reports ERANGE otherwise and
    // we fall back to the raw id rather than retrying blindly.
    std::string buf(4096, '\0');
    if (getpwuid_r(static_cast<uid_t>(uid), &pwd, buf.data(), buf.size(), &found) != 0
        || found == nullptr) {
        return {};
    }
    return QString::fromUtf8(found->pw_name);
#endif
}
