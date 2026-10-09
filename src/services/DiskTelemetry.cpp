#include "DiskTelemetry.h"

#ifdef _WIN32
#include <windows.h>

#include <pdh.h>
#include <pdhmsg.h>
#include <winioctl.h>
#elif defined(__linux__)
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <QFile>
#include <QTextStream>
#elif defined(__APPLE__)
#include <sys/mount.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#endif

#include <QElapsedTimer>

namespace {

constexpr int kPollMs{1000};

} // namespace

#ifdef _WIN32

// PDH physical-disk counters for the disk hosting the target volume.
// Computed counters need two collections; the first ticks warm up.
struct DiskTelemetry::PdhState {
    HQUERY query{nullptr};
    HCOUNTER active{nullptr};
    HCOUNTER queue{nullptr};
    HCOUNTER latency{nullptr};

    ~PdhState()
    {
        if (query != nullptr) {
            PdhCloseQuery(query);
        }
    }

    // Volume path -> physical disk number via extents (exact, one call).
    static bool diskNumberFor(const QString& targetPath, DWORD& disk)
    {
        std::wstring vol = targetPath.toStdWString();
        const std::size_t sep = vol.find_first_of(L"/\\", 2);
        const std::wstring root = (sep == std::wstring::npos) ? vol : vol.substr(0, sep + 1);
        HANDLE h = CreateFileW((L"\\\\.\\" + root.substr(0, 2)).c_str(), 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }
        struct HandleGuard {
            HANDLE h{nullptr};
            ~HandleGuard()
            {
                CloseHandle(h);
            }
        } guard{h};
        char out[512]{};
        DWORD got{0};
        if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, out,
                            sizeof(out), &got, nullptr)
            == 0) {
            return false;
        }
        const auto* extents = reinterpret_cast<VOLUME_DISK_EXTENTS*>(out);
        if (extents->NumberOfDiskExtents < 1) {
            return false;
        }
        disk = extents->Extents[0].DiskNumber;
        return true;
    }

    bool open(const QString& targetPath)
    {
        // Letter of the target volume, e.g. "C".
        std::wstring vol = targetPath.toStdWString();
        if (vol.size() < 2 || vol[1] != L':') {
            return false;
        }
        DWORD disk{0};
        if (!diskNumberFor(targetPath, disk)) {
            return false;
        }
        // Instance names look like "0 C:"; match by disk number plus the
        // volume letter suffix so naming variants cannot silently monitor
        // the wrong disk.
        const std::wstring want = L" " + std::wstring(1, vol[0]) + L":";
        DWORD listLen{0};
        DWORD countersLen{0};
        PdhEnumObjectItemsW(nullptr, nullptr, L"PhysicalDisk", nullptr, &countersLen,
                            nullptr, &listLen, PERF_DETAIL_WIZARD, 0);
        if (listLen < 4) {
            return false;
        }
        std::wstring instances(listLen, L'\0');
        if (PdhEnumObjectItemsW(nullptr, nullptr, L"PhysicalDisk", nullptr, &countersLen,
                                instances.data(), &listLen, PERF_DETAIL_WIZARD,
                                0)
            != ERROR_SUCCESS) {
            return false;
        }
        std::wstring match{};
        for (const wchar_t* p = instances.c_str(); *p != L'\0'; p += wcslen(p) + 1) {
            std::wstring inst{p};
            if (inst.size() >= want.size()
                && inst.compare(inst.size() - want.size(), want.size(), want) == 0) {
                const std::wstring prefix = std::to_wstring(disk) + L" ";
                if (inst.compare(0, prefix.size(), prefix) == 0) {
                    match = inst;
                    break;
                }
            }
        }
        if (match.empty()) {
            return false;
        }
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            return false;
        }
        const std::wstring base = L"\\PhysicalDisk(" + match + L")\\";
        const auto add = [&](const wchar_t* counter, HCOUNTER& out) {
            return PdhAddCounterW(query, (base + counter).c_str(), 0, &out) == ERROR_SUCCESS;
        };
        if (!add(L"% Disk Time", active) || !add(L"Avg. Disk Queue Length", queue)
            || !add(L"Avg. Disk sec/Transfer", latency)) {
            return false;
        }
        return PdhCollectQueryData(query) == ERROR_SUCCESS;
    }

    usa::DiskSample sample(bool warming)
    {
        usa::DiskSample out{};
        if (PdhCollectQueryData(query) != ERROR_SUCCESS || warming) {
            return out; // warming: need consecutive collections first
        }
        PDH_FMT_COUNTERVALUE value{};
        if (PdhGetFormattedCounterValue(active, PDH_FMT_DOUBLE, nullptr, &value)
                == ERROR_SUCCESS
            && value.CStatus == ERROR_SUCCESS) {
            out.activePct = std::min(value.doubleValue, 100.0);
        }
        if (PdhGetFormattedCounterValue(queue, PDH_FMT_DOUBLE, nullptr, &value)
                == ERROR_SUCCESS
            && value.CStatus == ERROR_SUCCESS) {
            out.queueDepth = std::max(value.doubleValue, 0.0);
        }
        if (PdhGetFormattedCounterValue(latency, PDH_FMT_DOUBLE, nullptr, &value)
                == ERROR_SUCCESS
            && value.CStatus == ERROR_SUCCESS) {
            out.latencyMs = value.doubleValue * 1000.0;
            out.available = true;
        }
        return out;
    }
};

#elif defined(__linux__)

// /proc/diskstats deltas for the exact device hosting the target path
// (st_dev major:minor match, partitions included).
struct DiskTelemetry::LinuxState {
    unsigned major{0};
    unsigned minor{0};
    std::uint64_t prevReads{0};
    std::uint64_t prevWrites{0};
    std::uint64_t prevReadMs{0};
    std::uint64_t prevWriteMs{0};
    std::uint64_t prevBusyMs{0};
    std::uint64_t prevWeightedMs{0};
    QElapsedTimer clock{};

    bool open(const QString& targetPath)
    {
        struct stat sb {};
        if (::stat(targetPath.toStdString().c_str(), &sb) != 0) {
            return false;
        }
        major = ::major(sb.st_dev);
        minor = ::minor(sb.st_dev);
        if (!readSelf()) {
            return false;
        }
        clock.start();
        return true;
    }

    // diskstats columns after the name: reads completed/merged, sectors
    // read, ms reading, writes completed/merged, sectors written,
    // ms writing, ios in progress, ms doing io, weighted ms doing io.
    bool readSelf()
    {
        QFile f{QStringLiteral("/proc/diskstats")};
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return false;
        }
        QTextStream in{&f};
        QString word{};
        while (!in.atEnd()) {
            bool ok{false};
            in >> word;
            const unsigned maj = word.toUInt(&ok);
            if (!ok) {
                in.readLine();
                continue;
            }
            in >> word;
            const unsigned min = word.toUInt(&ok);
            if (!ok) {
                in.readLine();
                continue;
            }
            in >> word; // name (matched by numbers, not by string)
            std::uint64_t v[11]{};
            bool good{true};
            for (int i{0}; i < 11; ++i) {
                in >> word;
                v[i] = word.toULongLong(&ok);
                if (!ok) {
                    good = false;
                    break;
                }
            }
            in.readLine();
            if (good && maj == major && min == minor) {
                prevReads = v[0];
                prevWrites = v[4];
                prevReadMs = v[3];
                prevWriteMs = v[7];
                prevBusyMs = v[9];
                prevWeightedMs = v[10];
                return true;
            }
        }
        return false;
    }

    usa::DiskSample sample(bool warming)
    {
        usa::DiskSample out{};
        const qint64 elapsed = clock.restart();
        std::uint64_t reads{0};
        std::uint64_t writes{0};
        std::uint64_t readMs{0};
        std::uint64_t writeMs{0};
        std::uint64_t busyMs{0};
        std::uint64_t weightedMs{0};
        // Re-read through a scratch copy so a vanishing device cannot
        // corrupt the baseline mid-computation.
        LinuxState probe{};
        probe.major = major;
        probe.minor = minor;
        if (warming || elapsed <= 0 || !probe.readSelf()) {
            return out;
        }
        reads = probe.prevReads;
        writes = probe.prevWrites;
        readMs = probe.prevReadMs;
        writeMs = probe.prevWriteMs;
        busyMs = probe.prevBusyMs;
        weightedMs = probe.prevWeightedMs;
        const double dt = static_cast<double>(elapsed);
        const std::uint64_t dIos = (reads - prevReads) + (writes - prevWrites);
        out.activePct = std::min(static_cast<double>(busyMs - prevBusyMs) / dt * 100.0, 100.0);
        out.queueDepth = static_cast<double>(weightedMs - prevWeightedMs) / dt;
        if (dIos > 0) {
            out.latencyMs = static_cast<double>((readMs - prevReadMs) + (writeMs - prevWriteMs))
                            / static_cast<double>(dIos);
        }
        out.available = true;
        prevReads = reads;
        prevWrites = writes;
        prevReadMs = readMs;
        prevWriteMs = writeMs;
        prevBusyMs = busyMs;
        prevWeightedMs = weightedMs;
        return out;
    }
};

#elif defined(__APPLE__)

// IOKit block-storage counters for the disk hosting the target volume.
// Best effort: every property lookup is null-checked, and any missing
// piece reports unavailable (never throttle blind) rather than guessing.
struct DiskTelemetry::MacState {
    io_service_t service{IO_OBJECT_NULL};
    std::uint64_t prevReadNs{0};
    std::uint64_t prevWriteNs{0};
    std::uint64_t prevIos{0};
    QElapsedTimer clock{};

    ~MacState()
    {
        if (service != IO_OBJECT_NULL) {
            IOObjectRelease(service);
        }
    }

    static QString diskOfVolume(const QString& targetPath)
    {
        // /dev/disk3s1 -> disk3 (whole disk, not the partition).
        struct statfs fs {};
        if (::statfs(targetPath.toStdString().c_str(), &fs) != 0) {
            return {};
        }
        QString dev = QString::fromUtf8(fs.f_mntfromname);
        if (!dev.startsWith(QStringLiteral("/dev/"))) {
            return {};
        }
        dev = dev.mid(5);
        const int s = dev.indexOf(QLatin1Char('s'), 4);
        return (s > 4) ? dev.left(s) : dev;
    }

    static std::uint64_t statNumber(CFDictionaryRef stats, CFStringRef key)
    {
        if (stats == nullptr || key == nullptr) {
            return 0;
        }
        CFNumberRef num = static_cast<CFNumberRef>(CFDictionaryGetValue(stats, key));
        if (num == nullptr || CFGetTypeID(num) != CFNumberGetTypeID()) {
            return 0;
        }
        std::uint64_t v{0};
        return CFNumberGetValue(num, kCFNumberSInt64Type, &v) ? v : 0;
    }

    bool open(const QString& targetPath)
    {
        const QString disk = diskOfVolume(targetPath);
        if (disk.isEmpty()) {
            return false;
        }
        io_iterator_t it{IO_OBJECT_NULL};
        if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                         IOServiceMatching(kIOBlockStorageDriverClassName),
                                         &it)
            != KERN_SUCCESS) {
            return false;
        }
        io_service_t svc{IO_OBJECT_NULL};
        while ((svc = IOIteratorNext(it)) != IO_OBJECT_NULL) {
            CFTypeRef name = IORegistryEntryCreateCFProperty(svc, CFSTR("BSD Name"),
                                                            kCFAllocatorDefault, 0);
            bool match{false};
            if (name != nullptr && CFGetTypeID(name) == CFStringGetTypeID()) {
                char buf[64]{};
                if (CFStringGetCString(static_cast<CFStringRef>(name), buf, sizeof(buf),
                                       kCFStringEncodingUTF8)) {
                    match = (disk == QString::fromUtf8(buf));
                }
                CFRelease(name);
            }
            if (match) {
                service = svc;
                break;
            }
            IOObjectRelease(svc);
        }
        IOObjectRelease(it);
        if (service == IO_OBJECT_NULL) {
            return false;
        }
        clock.start();
        return readSelf();
    }

    bool readSelf()
    {
        std::uint64_t r{0};
        std::uint64_t w{0};
        std::uint64_t ios{0};
        if (!readStats(r, w, ios)) {
            return false;
        }
        prevReadNs = r;
        prevWriteNs = w;
        prevIos = ios;
        return true;
    }

    bool readStats(std::uint64_t& readNs, std::uint64_t& writeNs, std::uint64_t& ios)
    {
        CFTypeRef stats = IORegistryEntryCreateCFProperty(service, CFSTR("Statistics"),
                                                          kCFAllocatorDefault, 0);
        if (stats == nullptr || CFGetTypeID(stats) != CFDictionaryGetTypeID()) {
            if (stats != nullptr) {
                CFRelease(stats);
            }
            return false;
        }
        CFDictionaryRef dict = static_cast<CFDictionaryRef>(stats);
        readNs = statNumber(dict, CFSTR("Total Time (Read)"));
        writeNs = statNumber(dict, CFSTR("Total Time (Write)"));
        const std::uint64_t reads = statNumber(dict, CFSTR("Reads"));
        const std::uint64_t writes = statNumber(dict, CFSTR("Writes"));
        CFRelease(stats);
        if (reads == 0 && writes == 0) {
            return false;
        }
        ios = reads + writes;
        return true;
    }

    usa::DiskSample sample(bool warming)
    {
        usa::DiskSample out{};
        const qint64 elapsed = clock.restart();
        std::uint64_t r{0};
        std::uint64_t w{0};
        std::uint64_t ios{0};
        if (warming || elapsed <= 0 || !readStats(r, w, ios)) {
            return out;
        }
        const double dtMs = static_cast<double>(elapsed);
        const std::uint64_t dIos = ios - prevIos;
        // Utilization approximated from busy-nanosecond deltas.
        out.activePct =
            std::min(static_cast<double>((r - prevReadNs) + (w - prevWriteNs)) / 1000000.0
                         / dtMs * 100.0,
                     100.0);
        if (dIos > 0) {
            out.latencyMs = static_cast<double>((r - prevReadNs) + (w - prevWriteNs))
                            / 1000000.0 / static_cast<double>(dIos);
        }
        out.queueDepth = -1.0; // IOKit exposes no queue counter: latency only
        out.available = true;
        prevReadNs = r;
        prevWriteNs = w;
        prevIos = ios;
        return out;
    }
};

#endif

DiskTelemetry::DiskTelemetry(QObject* parent)
    : QObject(parent)
{
    timer_.setInterval(kPollMs);
    connect(&timer_, &QTimer::timeout, this, &DiskTelemetry::poll);
}

DiskTelemetry::~DiskTelemetry()
{
    closeBackend();
}

void DiskTelemetry::startMonitoring(const QString& targetPath)
{
    stopMonitoring();
    targetPath_ = targetPath;
    warming_ = true;
#ifdef _WIN32
    pdh_ = new PdhState{};
    if (!pdh_->open(targetPath_)) {
        delete pdh_;
        pdh_ = nullptr;
    }
#elif defined(__linux__)
    linux_ = new LinuxState{};
    if (!linux_->open(targetPath_)) {
        delete linux_;
        linux_ = nullptr;
    }
#elif defined(__APPLE__)
    mac_ = new MacState{};
    if (!mac_->open(targetPath_)) {
        delete mac_;
        mac_ = nullptr;
    }
#endif
    timer_.start();
}

void DiskTelemetry::stopMonitoring()
{
    timer_.stop();
    closeBackend();
    targetPath_.clear();
}

void DiskTelemetry::closeBackend()
{
#ifdef _WIN32
    delete pdh_;
    pdh_ = nullptr;
#elif defined(__linux__)
    delete linux_;
    linux_ = nullptr;
#elif defined(__APPLE__)
    delete mac_;
    mac_ = nullptr;
#endif
}

void DiskTelemetry::poll()
{
#ifdef _WIN32
    if (pdh_ == nullptr) {
        emit sampleReady(usa::DiskSample{});
        return;
    }
    emit sampleReady(pdh_->sample(warming_));
#elif defined(__linux__)
    if (linux_ == nullptr) {
        emit sampleReady(usa::DiskSample{});
        return;
    }
    emit sampleReady(linux_->sample(warming_));
#elif defined(__APPLE__)
    if (mac_ == nullptr) {
        emit sampleReady(usa::DiskSample{});
        return;
    }
    emit sampleReady(mac_->sample(warming_));
#else
    emit sampleReady(usa::DiskSample{});
#endif
    warming_ = false;
}

