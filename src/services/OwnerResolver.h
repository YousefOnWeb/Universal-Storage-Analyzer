#pragma once

#include <QString>

#include <mutex>
#include <string>
#include <unordered_map>

// Backend-opaque owner ids (POSIX uid strings, volume owner ids) become
// human-readable display names. Resolution is lazy and cached per unique
// id for the process lifetime: name lookups can hit the directory server,
// so they run once per id, off the scan hot path, behind this mutex.
// "(unknown)" passes through untouched — it is a coverage statement,
// not an id. Thread-safe; owned by the worker bridge.
class OwnerResolver {
public:
    OwnerResolver() = default;
    OwnerResolver(const OwnerResolver&) = delete;
    OwnerResolver& operator=(const OwnerResolver&) = delete;

    QString displayName(const std::string& ownerId);

private:
    static QString resolve(const std::string& ownerId);

    std::mutex mutex_{};
    std::unordered_map<std::string, QString> cache_{};
};
