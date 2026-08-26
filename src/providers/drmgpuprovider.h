#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

#include "gpuprovider.h"

// Generic DRM/sysfs backend: amdgpu, i915, xe and anything else implementing
// the kernel's DRM fdinfo interface. Needs no optional tools at all.
//
// Aggregate figures come from sysfs where the driver exposes them (amdgpu's
// gpu_busy_percent / vcn_busy_percent) and are otherwise derived from the
// same per-client engine counters that feed the process columns.
//
// Per-process figures come from /proc/PID/fdinfo. Three properties of that
// interface drive the implementation:
//
//   * Engine counters are cumulative nanoseconds, so utilization is a delta
//     over elapsed wall time.
//   * A driver omits any engine whose usage is zero — an idle GPU emits no
//     drm-engine-* lines at all. Absence therefore means zero, but only for
//     drivers known to implement engine stats; see kEngineStatsDrivers.
//   * One process can hold several fds onto the same DRM client, and each
//     repeats that client's counters, so clients must be deduplicated by
//     drm-client-id before anything is summed.
//
// NVIDIA's proprietary driver is a deliberate non-participant: its DRM fds
// carry no drm-* keys at all (verified on 610.57.04), so cards bound to it
// are reported with metricsAvailable=false and left to the nvidia-smi
// backend.
class DrmGpuProvider final : public IGpuProvider
{
public:
    DrmGpuProvider();

    bool isAvailable() const override;
    QVector<GpuDeviceInfo> devices() const override;
    QVector<GpuSnapshot> sample() override;
    QHash<int, GpuProcessUsage> sampleProcesses() override;
    void setProcessSamplingEnabled(bool enabled) override;

private:
    struct Device {
        GpuDeviceInfo info;
        QString sysfsPath; // /sys/class/drm/cardN/device
        QString hwmonPath; // .../hwmon/hwmonN, empty when absent
        bool engineStatsExpected = false;
    };

    // Cumulative per-engine nanoseconds for one DRM client. Kept per engine
    // rather than pre-reduced: utilization is the max delta across the
    // engines in a category, and a max of cumulative counters would stall
    // whenever a client moved its work from a long-used engine to a fresh
    // one.
    using EngineCounters = QHash<QByteArray, quint64>;

    // What one fdinfo file described.
    struct FdInfoEntry {
        QString deviceId;
        QByteArray clientId;
        EngineCounters engineNs;
        bool valid = false;
    };

    void discoverDevices();
    // Single /proc walk feeding both the per-process map and the derived
    // aggregate figures. Runs when either consumer needs it.
    void refreshFromFdInfo();
    bool parseFdInfo(const QByteArray &data, const QString &nodeName,
                     FdInfoEntry &out) const;

    QVector<Device> m_devices;
    // Maps "renderD129" / "card0" to a device id, for drivers that omit
    // drm-pdev from fdinfo.
    QHash<QString, QString> m_nodeToDevice;

    QHash<QString, EngineCounters> m_previousClients; // key: deviceId/clientId
    QHash<int, GpuProcessUsage> m_processUsage;
    QHash<QString, double> m_derivedBusyPercent;  // by device id
    QHash<QString, double> m_derivedVideoPercent; // by device id

    qint64 m_previousWalkMsec = 0;
    bool m_processSamplingEnabled = false;
    bool m_needDerivedAggregate = false;
    bool m_havePreviousClients = false;
};
