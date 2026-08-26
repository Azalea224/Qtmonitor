#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>

#include "gpuprovider.h"

class QProcess;

// NVIDIA backend, driven by nvidia-smi.
//
// The proprietary driver exposes no utilization sysfs nodes and writes no
// drm-* keys into fdinfo, so shelling out is the only route to these numbers
// without linking NVML. nvidia-smi is treated as a strictly optional tool:
// when it is absent this backend reports no devices and the card is left to
// the DRM backend, which lists it with an install hint.
//
// Both invocations run asynchronously. They are quick (~13ms for the device
// query, ~21ms for pmon) but that is still one to two dropped frames if run
// on the GUI thread every second, so sample() starts a process and returns
// the previous result instead of waiting. Data therefore lags one tick,
// which is imperceptible at a 1s cadence.
class NvidiaSmiGpuProvider final : public QObject, public IGpuProvider
{
    Q_OBJECT

public:
    explicit NvidiaSmiGpuProvider(QObject *parent = nullptr);

    bool isAvailable() const override;
    QVector<GpuDeviceInfo> devices() const override;
    QVector<GpuSnapshot> sample() override;
    QHash<int, GpuProcessUsage> sampleProcesses() override;
    void setProcessSamplingEnabled(bool enabled) override;

private:
    // nvidia-smi writes a PCI address with an eight-digit domain
    // ("00000000:01:00.0") while sysfs uses four ("0000:01:00.0"). Device
    // identity is shared across backends, so one form has to win: sysfs's.
    static QString normalizeBusId(const QString &busId);

    void discoverDevices();
    void startQuery();
    void startProcessQuery();
    void parseQueryOutput(const QByteArray &output);
    void parsePmonOutput(const QByteArray &output);

    QProcess *m_queryProcess;
    QProcess *m_pmonProcess;
    QElapsedTimer m_queryStarted;
    QElapsedTimer m_pmonStarted;

    QVector<GpuDeviceInfo> m_devices;
    QVector<GpuSnapshot> m_snapshots;
    QHash<int, GpuProcessUsage> m_processUsage;

    bool m_available = false;
    bool m_processSamplingEnabled = false;
};
