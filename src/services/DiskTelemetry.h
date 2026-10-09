#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>

// OS-level physical disk signals for the engine throttle policy. One
// sample per second from a dedicated worker thread; the engine consumes
// them through DirectConnection-safe primitives only (same contract as
// pause/cancel). Backends are per-OS behind this interface; when the
// platform cannot observe (or the volume cannot be mapped to a disk),
// samples report available=false and the engine never throttles.
namespace usa {

struct DiskSample {
    bool available{false};
    double latencyMs{-1.0}; // average transfer latency; <0 when unknown
    double queueDepth{0.0}; // average queue length
    double activePct{0.0}; // disk active time percent, 0-100
};

} // namespace usa

Q_DECLARE_METATYPE(usa::DiskSample)

class DiskTelemetry : public QObject {
    Q_OBJECT
public:
    explicit DiskTelemetry(QObject* parent = nullptr);
    ~DiskTelemetry() override;

    DiskTelemetry(const DiskTelemetry&) = delete;
    DiskTelemetry& operator=(const DiskTelemetry&) = delete;

public slots:
    void startMonitoring(const QString& targetPath);
    void stopMonitoring();

signals:
    void sampleReady(usa::DiskSample sample);

private slots:
    void poll();

private:
    void closeBackend();

    QTimer timer_{};
    QString targetPath_{};
    bool warming_{true};
#ifdef _WIN32
    struct PdhState;
    PdhState* pdh_{nullptr};
#elif defined(__linux__)
    struct LinuxState;
    LinuxState* linux_{nullptr};
#elif defined(__APPLE__)
    struct MacState;
    MacState* mac_{nullptr};
#endif
};
