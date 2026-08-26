#include "gpuprovider.h"

#include "drmgpuprovider.h"
#include "nvidiagpuprovider.h"

GpuRegistry::GpuRegistry()
{
    // Order matters: the DRM backend enumerates every card in sysfs order,
    // which is the order the sidebar shows them in. The nvidia-smi backend
    // then supersedes the entries for cards it can actually measure, without
    // moving them.
    auto drm = std::make_unique<DrmGpuProvider>();
    if (drm->isAvailable()) {
        m_providers.push_back(std::move(drm));
    }

    auto nvidia = std::make_unique<NvidiaSmiGpuProvider>();
    if (nvidia->isAvailable()) {
        m_providers.push_back(std::move(nvidia));
    }

    rebuildDeviceList();
}

GpuRegistry::~GpuRegistry() = default;

void GpuRegistry::rebuildDeviceList()
{
    m_devices.clear();
    QHash<QString, int> indexById;

    for (const auto &provider : m_providers) {
        const QVector<GpuDeviceInfo> devices = provider->devices();
        for (const GpuDeviceInfo &info : devices) {
            const auto existing = indexById.constFind(info.id);
            if (existing == indexById.cend()) {
                indexById.insert(info.id, m_devices.size());
                m_devices.append(info);
                continue;
            }
            // Same physical card seen twice. The backend that can measure it
            // wins; its name is better sourced too (nvidia-smi's marketing
            // name beats the pci.ids codename).
            GpuDeviceInfo &current = m_devices[*existing];
            if (info.metricsAvailable && !current.metricsAvailable) {
                current = info;
            }
        }
    }
}

QVector<GpuSnapshot> GpuRegistry::sample()
{
    QVector<GpuSnapshot> merged;
    QHash<QString, int> indexById;

    for (const auto &provider : m_providers) {
        const QVector<GpuSnapshot> snapshots = provider->sample();
        for (const GpuSnapshot &snapshot : snapshots) {
            if (indexById.contains(snapshot.id)) {
                continue; // only one backend measures any given card
            }
            indexById.insert(snapshot.id, merged.size());
            merged.append(snapshot);
        }
    }
    return merged;
}

QHash<int, GpuProcessUsage> GpuRegistry::sampleProcesses()
{
    QHash<int, GpuProcessUsage> merged;

    for (const auto &provider : m_providers) {
        const QHash<int, GpuProcessUsage> usage = provider->sampleProcesses();
        for (auto it = usage.cbegin(); it != usage.cend(); ++it) {
            GpuProcessUsage &slot = merged[it.key()];
            // A process can drive several GPUs at once and the column shows
            // one figure, so measured shares add. A negative reading means
            // "that backend could not tell" and contributes nothing, but any
            // measurement at all lifts the entry out of unknown.
            if (it->graphicsPercent >= 0.0) {
                slot.graphicsPercent =
                    qMax(slot.graphicsPercent, 0.0) + it->graphicsPercent;
            }
            if (it->videoPercent >= 0.0) {
                slot.videoPercent = qMax(slot.videoPercent, 0.0) + it->videoPercent;
            }
        }
    }

    for (auto it = merged.begin(); it != merged.end(); ++it) {
        it->graphicsPercent = qMin(100.0, it->graphicsPercent);
        it->videoPercent = qMin(100.0, it->videoPercent);
    }
    return merged;
}

void GpuRegistry::setProcessSamplingEnabled(bool enabled)
{
    for (const auto &provider : m_providers) {
        provider->setProcessSamplingEnabled(enabled);
    }
}
