#pragma once

#include <QObject>

#include "providers/cpuprovider.h"
#include "providers/diskprovider.h"
#include "providers/gpuprovider.h"
#include "providers/hwmonprovider.h"
#include "providers/memoryprovider.h"
#include "providers/networkprovider.h"
#include "providers/processprovider.h"

class QTimer;

// Drives all data providers on a single QTimer and emits fresh snapshots.
// Lives on the GUI thread; providers are synchronous /proc reads which are
// cheap enough at a 1s cadence, except the GPU backends, which do their own
// asynchronous work rather than blocking here.
class Sampler : public QObject
{
    Q_OBJECT

public:
    explicit Sampler(QObject *parent = nullptr);

    void setInterval(int msec);
    int interval() const;
    void start();
    void stop();

    // Walking every /proc/PID costs far more than the CPU and memory reads,
    // so it is skipped unless something is actually showing the process list.
    void setProcessSamplingEnabled(bool enabled);

    // Disks, network interfaces and sensors are only read while the
    // Performance tab is on screen: their pages are the only consumers, the
    // mount-usage side of the disk provider does real filesystem work, and an
    // hwmon read is a hardware transaction costing milliseconds.
    void setResourceSamplingEnabled(bool enabled);

    // GPUs, disks and interfaces are all enumerated once at construction; the
    // UI builds a page per device from these lists.
    const GpuRegistry &gpus() const { return m_gpuRegistry; }
    QVector<DiskDeviceInfo> disks() const { return m_diskProvider.devices(); }
    QVector<NetworkDeviceInfo> networkInterfaces() const
    {
        return m_networkProvider.devices();
    }
    // hwmon's chips followed by any GPU that hwmon cannot see; see gpusensors.
    QVector<SensorChipInfo> sensorChips() const
    {
        return m_sensorProvider.chips() + m_gpuSensorChips;
    }

signals:
    void cpuSampled(const CpuSnapshot &snapshot);
    void memorySampled(const MemorySnapshot &snapshot);
    void gpuSampled(const QVector<GpuSnapshot> &snapshots);
    void diskSampled(const QVector<DiskSnapshot> &snapshots);
    void networkSampled(const QVector<NetworkSnapshot> &snapshots);
    void sensorsSampled(const QVector<SensorChipSnapshot> &snapshots);
    void processesSampled(const ProcessSnapshot &snapshot);

private:
    void tick();
    // Process rows carry GPU shares that come from a different provider, so
    // the two are joined by pid here rather than inside either one.
    ProcessSnapshot sampleProcessesWithGpu();
    // hwmon readings plus the GPU-derived chips, from the GPU snapshots this
    // tick already took rather than a second nvidia-smi round trip.
    QVector<SensorChipSnapshot> sampleSensors();

    QTimer *m_timer;
    ProcfsCpuProvider m_cpuProvider;
    ProcfsMemoryProvider m_memoryProvider;
    ProcfsProcessProvider m_processProvider;
    ProcfsDiskProvider m_diskProvider;
    ProcfsNetworkProvider m_networkProvider;
    HwmonSensorProvider m_sensorProvider;
    GpuRegistry m_gpuRegistry;
    QVector<SensorChipInfo> m_gpuSensorChips;
    QVector<GpuSnapshot> m_lastGpuSnapshots;
    bool m_processSamplingEnabled = false;
    bool m_resourceSamplingEnabled = false;
};
