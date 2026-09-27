#include "gpusensors.h"

#include <QCoreApplication>

#include <algorithm>

namespace {

using Kind = SensorChannelInfo::Kind;

// Chip ids share a namespace with hwmon's "hwmonN", so they are prefixed.
const QString kChipPrefix = QStringLiteral("gpu:");

const QString kTemperatureChannel = QStringLiteral("temp1");
const QString kPowerChannel = QStringLiteral("power1");
const QString kFanChannel = QStringLiteral("fan1");

SensorChannelInfo channel(Kind kind, const QString &id, const QString &label)
{
    SensorChannelInfo info;
    info.kind = kind;
    info.id = id;
    info.label = label;
    return info;
}

} // namespace

namespace gpusensors {

QVector<SensorChipInfo> chips(const QVector<GpuDeviceInfo> &gpus,
                              const QVector<SensorChipInfo> &hwmonChips)
{
    QVector<SensorChipInfo> result;
    for (const GpuDeviceInfo &gpu : gpus) {
        // Only the proprietary NVIDIA driver is known to measure a card that
        // has no hwmon node. Everything the DRM backend reports as a
        // temperature comes from hwmon in the first place, so any other GPU
        // missing from hwmon — an i915 iGPU, say — would get a box of em
        // dashes and nothing else.
        if (!gpu.metricsAvailable || !gpu.driver.startsWith(QLatin1String("nvidia"))) {
            continue;
        }
        const bool coveredByHwmon =
            std::any_of(hwmonChips.cbegin(), hwmonChips.cend(),
                        [&gpu](const SensorChipInfo &chip) {
                            return chip.deviceId == gpu.id;
                        });
        if (coveredByHwmon) {
            continue;
        }

        SensorChipInfo chip;
        chip.id = kChipPrefix + gpu.id;
        // Shown beside the display name where hwmon chips show their driver,
        // so a reading can be lined up against its source.
        chip.name = QStringLiteral("nvidia-smi");
        // The model name rather than "GPU": the amdgpu iGPU's hwmon chip is
        // already called "GPU", and two chart lines with one name would be
        // indistinguishable.
        chip.displayName = gpu.name;
        chip.alsoShownIn =
            QCoreApplication::translate("SensorProvider", "the GPU page");
        chip.deviceId = gpu.id;
        chip.channels = {
            channel(Kind::Temperature, kTemperatureChannel,
                    QCoreApplication::translate("SensorProvider", "Core")),
            channel(Kind::Power, kPowerChannel,
                    QCoreApplication::translate("SensorProvider", "Board power")),
            channel(Kind::FanDuty, kFanChannel,
                    QCoreApplication::translate("SensorProvider", "Fan")),
        };
        result.append(chip);
    }
    return result;
}

QVector<SensorChipSnapshot> snapshots(const QVector<GpuSnapshot> &gpuSnapshots,
                                      const QVector<SensorChipInfo> &gpuChips)
{
    QVector<SensorChipSnapshot> result;
    result.reserve(gpuChips.size());
    for (const SensorChipInfo &chip : gpuChips) {
        const auto gpu = std::find_if(gpuSnapshots.cbegin(), gpuSnapshots.cend(),
                                      [&chip](const GpuSnapshot &snapshot) {
                                          return snapshot.id == chip.deviceId;
                                      });
        const bool found = gpu != gpuSnapshots.cend();

        SensorChipSnapshot snapshot;
        snapshot.chipId = chip.id;
        snapshot.readings = {
            {kTemperatureChannel, found ? gpu->temperatureC : -1.0},
            {kPowerChannel, found ? gpu->powerWatts : -1.0},
            {kFanChannel, found ? gpu->fanPercent : -1.0},
        };
        result.append(snapshot);
    }
    return result;
}

} // namespace gpusensors
