#pragma once

#include <QVector>

#include "gpuprovider.h"
#include "hwmonprovider.h"

// Sensor chips for GPUs the kernel's hwmon interface cannot see.
//
// NVIDIA's proprietary driver registers no hwmon device at all, so on a box
// with one the Sensors page would silently omit what is usually the hottest
// part in the machine. The GPU backends already read its temperature, power
// and fan for the GPU page; these functions present those same numbers in the
// Sensors page's own shape, so the page needs no GPU-specific code.
//
// A GPU that does have a hwmon chip (amdgpu, nouveau, a discrete i915/xe) is
// skipped, matched by PCI address: it already reaches the page through hwmon,
// with limits and labels nvidia-smi cannot supply.
namespace gpusensors {

// Called once at startup, after both device lists are enumerated.
QVector<SensorChipInfo> chips(const QVector<GpuDeviceInfo> &gpus,
                              const QVector<SensorChipInfo> &hwmonChips);

// Maps the GPU backends' latest snapshots onto `gpuChips`. A card missing
// from `gpuSnapshots` yields readings of -1 rather than no chip, so its box
// shows em dashes instead of freezing on stale values.
QVector<SensorChipSnapshot> snapshots(const QVector<GpuSnapshot> &gpuSnapshots,
                                      const QVector<SensorChipInfo> &gpuChips);

} // namespace gpusensors
