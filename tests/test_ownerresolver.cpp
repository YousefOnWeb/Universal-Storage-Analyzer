#include <QtTest>

#include <QString>

#include <string>

#ifdef _WIN32
#include <windows.h>

#include <sddl.h>
#else
#include <string>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "../src/services/OwnerResolver.h"

// Lazy cached owner-id -> display-name resolution. Current-user case
// runs the real OS lookup on every platform; bogus ids prove fallback.
class TestOwnerResolver : public QObject {
    Q_OBJECT
private slots:
    void unknownPassesThrough()
    {
        OwnerResolver resolver{};
        QCOMPARE(resolver.displayName("(unknown)"), QStringLiteral("(unknown)"));
        QCOMPARE(resolver.displayName(""), QStringLiteral("(unknown)"));
    }

    void resolvesCurrentUser()
    {
        const std::string me = currentOwnerId();
        QVERIFY(!me.empty());
        OwnerResolver resolver{};
        const QString name = resolver.displayName(me);
        QVERIFY(!name.isEmpty());
        // Second call serves the cache; result identical.
        QCOMPARE(resolver.displayName(me), name);
    }

    void bogusIdFallsBackToRaw()
    {
#ifdef _WIN32
        const std::string bogus{"S-1-5-9999-8888-7777"};
#else
        const std::string bogus{"not-a-user"};
#endif
        OwnerResolver resolver{};
        QCOMPARE(resolver.displayName(bogus), QString::fromStdString(bogus));
    }

private:
    static std::string currentOwnerId()
    {
#ifdef _WIN32
        HANDLE rawToken{};
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken) == 0) {
            return {};
        }
        struct HandleGuard {
            HANDLE h{nullptr};
            ~HandleGuard()
            {
                if (h != nullptr) {
                    CloseHandle(h);
                }
            }
        } guard{rawToken};
        DWORD needed{0};
        GetTokenInformation(rawToken, TokenUser, nullptr, 0, &needed);
        if (needed == 0) {
            return {};
        }
        std::string buf(needed, '\0');
        if (GetTokenInformation(rawToken, TokenUser, buf.data(), needed, &needed) == 0) {
            return {};
        }
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buf.data());
        wchar_t* sidStr{nullptr};
        if (ConvertSidToStringSidW(user->User.Sid, &sidStr) == 0 || sidStr == nullptr) {
            return {};
        }
        const QString sid = QString::fromWCharArray(sidStr);
        LocalFree(sidStr);
        return sid.toStdString();
#else
        return std::to_string(static_cast<unsigned long>(::getuid()));
#endif
    }
};

QTEST_MAIN(TestOwnerResolver)
#include "test_ownerresolver.moc"
