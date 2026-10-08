#include <QtTest>

#include <string>

#include "../src/services/scanner/Utf8.h"

using usa::scanner::detail::pathToUtf8;
using usa::scanner::detail::utf16ToUtf8;

// The converter must never throw (a throw in an engine worker thread
// terminates the process) and must preserve valid input byte-for-byte.
class TestUtf8 : public QObject {
    Q_OBJECT
private slots:
    void convertsAsciiUnchanged()
    {
        QCOMPARE(utf16ToUtf8(L"hello.txt"), std::string{"hello.txt"});
    }

    void convertsBmpAndAstral()
    {
        // é (U+00E9) -> C3 A9 ; 😀 (U+1F600) -> F0 9F 98 80.
        const std::wstring mixed = {L'a', static_cast<wchar_t>(0xE9), static_cast<wchar_t>(0xD83D),
                                    static_cast<wchar_t>(0xDE00)};
        const std::string out = utf16ToUtf8(mixed);
        QCOMPARE(out, std::string{"a\xC3\xA9\xF0\x9F\x98\x80"});
    }

    void replacesLoneSurrogates()
    {
        // Lone high / lone low surrogates become U+FFFD (EF BF BD), no throw.
        const std::string loneHigh = utf16ToUtf8(std::wstring{static_cast<wchar_t>(0xD800)});
        const std::string loneLow = utf16ToUtf8(std::wstring{static_cast<wchar_t>(0xDC00)});
        QCOMPARE(loneHigh, std::string{"\xEF\xBF\xBD"});
        QCOMPARE(loneLow, std::string{"\xEF\xBF\xBD"});
        QCOMPARE(utf16ToUtf8(std::wstring{}), std::string{});
    }
};

QTEST_MAIN(TestUtf8)
#include "test_utf8.moc"
