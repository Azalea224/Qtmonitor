#include "sampler.h"

#include <QTimer>

#include "providers/gpusensors.h"

Sampler::Sampler(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
    , m_gpuSensorChips(gpusensors::chips(m_gpuRegistry.devices(), m_sensorProvider.chips()))
{
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
}

void Sampler::setInterval(int msec)
{
    m_timer->setInterval(msec);
}

int Sampler::interval() const
{
    return m_timer->interval();
}

void Sampler::start()
{
    tick(); // immediate first sample so the UI isn't empty for a second
    m_timer->start();
}

void Sampler::stop()
{
    m_timer->stop();
}

void Sampler::setProcessSamplingEnabled(bool enabled)
{
    if (m_processSamplingEnabled == enabled) {
        return;
    }
    m_processSamplingEnabled = enabled;
    m_gpuRegistry.setProcessSamplingEnabled(enabled);
    if (enabled && m_timer->isActive()) {
        // Populate immediately instead of leaving an empty table until the
        // next tick. Rates in this first sample span the whole idle gap,
        // which self-corrects on the following tick.
        emit processesSampled(sampleProcessesWithGpu());
    }
}

void Sampler::setResourceSamplingEnabled(bool enabled)
{
    if (m_resourceSamplingEnabled == enabled) {
        return;
    }
    m_resourceSamplingEnabled = enabled;
    if (enabled && m_timer->isActive()) {
        // Same reasoning as the process gate: populate at once rather than
        // leaving empty charts for a tick. The first rates after an idle gap
        // span that whole gap, which self-corrects immediately.
        emit diskSampled(m_diskProvider.sample());
        emit networkSampled(m_networkProvider.sample());
        emit sensorsSampled(sampleSensors());
    }
}

ProcessSnapshot Sampler::sampleProcessesWithGpu()
{
    ProcessSnapshot snapshot = m_processProvider.sample();

    // Reflects the walk the GPU registry did during this tick's sample();
    // processes absent from the map keep the -1 that means "not measured".
    const QHash<int, GpuProcessUsage> gpuUsage = m_gpuRegistry.sampleProcesses();
    if (gpuUsage.isEmpty()) {
        return snapshot;
    }

    for (ProcessInfo &process : snapshot.processes) {
        const auto usage = gpuUsage.constFind(process.pid);
        if (usage == gpuUsage.cend()) {
            continue;
        }
        process.gpuComputePercent = usage->graphicsPercent;
        process.gpuVideoPercent = usage->videoPercent;
    }
    return snapshot;
}

QVector<SensorChipSnapshot> Sampler::sampleSensors()
{
    return m_sensorProvider.sample()
        + gpusensors::snapshots(m_lastGpuSnapshots, m_gpuSensorChips);
}

void Sampler::tick()
{
    emit cpuSampled(m_cpuProvider.sample());
    emit memorySampled(m_memoryProvider.sample());

    // Must precede sampleProcesses(): one /proc walk feeds both the device
    // totals and the per-process shares.
    m_lastGpuSnapshots = m_gpuRegistry.sample();
    emit gpuSampled(m_lastGpuSnapshots);

    if (m_resourceSamplingEnabled) {
        emit diskSampled(m_diskProvider.sample());
        emit networkSampled(m_networkProvider.sample());
        // The provider re-serves its last readings if called again too soon,
        // so this stays cheap at the 0.5 s speed without the page having to
        // know anything about it.
        emit sensorsSampled(sampleSensors());
    }

    if (m_processSamplingEnabled) {
        emit processesSampled(sampleProcessesWithGpu());
    }
}
