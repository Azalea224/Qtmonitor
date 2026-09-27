#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QVector>

// One measurement channel on one chip: "temp1" on k10temp, "fan2" on a
// superio controller. Everything here is fixed for the life of the process —
// the live number arrives separately in a SensorReading.
struct SensorChannelInfo {
    enum class Kind {
        Temperature,
        Fan,
        Voltage,
        Power,
        Current,
        // Fan duty as a percentage of the fan's maximum. hwmon itself never
        // produces this (its fans are RPM); it exists for nvidia-smi, which
        // reports the NVIDIA card's fan only as a percentage. See gpusensors.
        FanDuty,
    };

    Kind kind = Kind::Temperature;
    QString id;    // sysfs channel prefix, "temp1"
    QString label; // from <channel>_label when the chip supplies one

    // Manufacturer limits, in the same display units as the reading, or -1
    // when the chip does not publish them. They are genuinely optional:
    // k10temp on this box reports neither _crit nor _max, so anything that
    // colours a value by proximity to its limit has to treat their absence as
    // normal rather than as a reason to show nothing.
    double criticalLimit = -1.0;
    double highLimit = -1.0;
};

// A hwmon chip as the UI presents it. Identity is the sysfs directory name,
// which is the only thing stable across a single boot; the chip `name` is not
// unique (this box has two spd5118 chips, one per DIMM).
struct SensorChipInfo {
    QString id;          // "hwmon1"
    QString name;        // raw chip name from /sys/class/hwmon/*/name
    QString displayName; // curated, e.g. "CPU"; numbered when ambiguous
    // Non-empty when this chip's readings also appear on another page, so the
    // duplication reads as deliberate rather than as a bug.
    QString alsoShownIn;
    // Basename of the chip's `device` link: the PCI address for a GPU
    // ("0000:0e:00.0"), empty when the chip has no parent device. It is what
    // lets a GPU that already has a hwmon chip be told apart from one that
    // does not — see gpusensors.
    QString deviceId;
    QVector<SensorChannelInfo> channels;
};

struct SensorReading {
    QString channelId;
    // Display units: °C, RPM, volts, watts, amps. -1 means the channel exists
    // but could not be read this tick — the established "never show a
    // misleading 0" rule, same as /proc/PID/io on another user's process.
    double value = -1.0;
};

struct SensorChipSnapshot {
    QString chipId;
    QVector<SensorReading> readings;
};

// Abstract sensor source. Named ISensorProvider because that is what the
// original architecture sketch called it, back when the assumption was that
// it would shell out to lm_sensors.
class ISensorProvider
{
public:
    virtual ~ISensorProvider() = default;
    virtual bool isAvailable() const = 0;
    // Enumerated once at construction, like disks, GPUs and interfaces.
    virtual QVector<SensorChipInfo> chips() const = 0;
    virtual QVector<SensorChipSnapshot> sample() = 0;
};

// Default — and only — sensor provider: the kernel's hwmon sysfs interface.
//
// This deliberately does NOT shell out to `sensors -j`. Everything lm_sensors
// reports comes from /sys/class/hwmon, which is world-readable and needs no
// tool, no daemon and no elevation; drmgpuprovider.cpp has been reading it
// directly since Phase 4. Adding lm_sensors as an optional dependency would
// have bought nothing except an asterisk on the zero-dependency promise, so
// the `Sensors` entry in optionaltools was deleted when this landed rather
// than finally implemented.
class HwmonSensorProvider final : public ISensorProvider
{
public:
    // `root` is prepended to every /sys path; empty is the real filesystem.
    // See ProcfsCpuProvider for why this exists.
    explicit HwmonSensorProvider(QString root = QString());

    bool isAvailable() const override;
    QVector<SensorChipInfo> chips() const override { return m_chips; }
    QVector<SensorChipSnapshot> sample() override;

private:
    void enumerateChips();
    void assignDisplayNames();

    QString m_root;
    QVector<SensorChipInfo> m_chips;
    // Absolute path per chip id, resolved once so a sample is a plain file
    // read per channel with no directory work.
    QVector<QString> m_chipPaths;

    // An hwmon read is a hardware transaction, not a memory read, and the
    // slowest chips here cost over a millisecond each. Readings are held and
    // re-served if asked for again too soon; see sample() for the numbers and
    // for why the floor sits just under a second.
    QElapsedTimer m_sinceLastRead;
    QVector<SensorChipSnapshot> m_lastSnapshots;
    static constexpr qint64 kMinSampleIntervalMsec = 900;
};
