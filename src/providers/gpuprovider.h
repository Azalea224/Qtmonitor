#pragma once

#include <QHash>
#include <QString>
#include <QVector>

#include <memory>
#include <vector>

// A GPU as the UI presents it. Identity is the PCI address because every
// backend can report it (sysfs directly, nvidia-smi as pci.bus_id), which is
// what lets two backends describing the same card be merged.
struct GpuDeviceInfo {
    QString id;     // "0000:01:00.0"
    QString name;   // "NVIDIA GeForce RTX 3090 Ti"
    QString driver; // "nvidia", "amdgpu", "i915", "xe", ...
    quint64 memTotalBytes = 0;
    // A card can be visible in sysfs while nothing is able to report metrics
    // for it — an NVIDIA card without nvidia-utils installed. It still gets a
    // sidebar row carrying this hint instead of graphs, rather than silently
    // vanishing and leaving the user wondering where their GPU went.
    bool metricsAvailable = false;
    QString unavailableHint;
};

// Live per-device counters. A negative value means this backend cannot
// measure that quantity at all, which the UI renders as an em dash; it is
// deliberately distinct from a measured zero.
struct GpuSnapshot {
    QString id;
    double busyPercent = -1.0;
    double videoPercent = -1.0;
    quint64 memUsedBytes = 0;
    quint64 memTotalBytes = 0;
    double temperatureC = -1.0;
    double powerWatts = -1.0;
    double clockMhz = -1.0;
    // Percent of the fan's maximum duty, not RPM: nvidia-smi reports nothing
    // else. -1 on a fanless card and on every DRM-backed one, whose fans are
    // hwmon channels and reach the Sensors page that way.
    double fanPercent = -1.0;
};

// Per-process share, merged into ProcessInfo by the sampler. Negative means
// unknown for the same reason as above.
struct GpuProcessUsage {
    double graphicsPercent = -1.0;
    double videoPercent = -1.0;
};

// Abstract GPU metrics source.
//
// Unlike the CPU and memory providers, a GPU backend may be built on a
// subprocess rather than a /proc read. sample() must therefore never block:
// implementations kick off whatever asynchronous work they need and return
// the freshest data they already hold, which for subprocess-backed providers
// means results lag by one tick. At a 1s cadence that is invisible, and it
// keeps the GUI thread free of multi-millisecond stalls.
class IGpuProvider
{
public:
    virtual ~IGpuProvider() = default;

    virtual bool isAvailable() const = 0;

    // Devices this backend knows about. Re-queried by the registry only at
    // startup: hotplugging a GPU is not a case this app tries to handle.
    virtual QVector<GpuDeviceInfo> devices() const = 0;

    virtual QVector<GpuSnapshot> sample() = 0;

    // Keyed by pid. Only populated while process sampling is enabled.
    virtual QHash<int, GpuProcessUsage> sampleProcesses() = 0;

    // Per-process GPU accounting costs far more than the aggregate figures
    // (an fd walk, or a second subprocess), so it follows the same gate as
    // the /proc process walk.
    virtual void setProcessSamplingEnabled(bool enabled) = 0;
};

// Owns every GPU backend and presents them as one device list.
//
// Backends overlap: an NVIDIA card appears both in sysfs (as a DRM node the
// generic backend can see but not measure) and in nvidia-smi (fully
// measurable). Merging by PCI address keeps one row per physical card and
// lets the backend that can actually measure it win.
class GpuRegistry
{
public:
    GpuRegistry();
    ~GpuRegistry();

    GpuRegistry(const GpuRegistry &) = delete;
    GpuRegistry &operator=(const GpuRegistry &) = delete;

    bool hasDevices() const { return !m_devices.isEmpty(); }
    const QVector<GpuDeviceInfo> &devices() const { return m_devices; }

    QVector<GpuSnapshot> sample();
    QHash<int, GpuProcessUsage> sampleProcesses();
    void setProcessSamplingEnabled(bool enabled);

private:
    void rebuildDeviceList();

    std::vector<std::unique_ptr<IGpuProvider>> m_providers;
    QVector<GpuDeviceInfo> m_devices;
};
