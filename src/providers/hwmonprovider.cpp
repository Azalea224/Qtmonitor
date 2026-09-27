#include "hwmonprovider.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace {

using Kind = SensorChannelInfo::Kind;

QString readSysfsLine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readLine()).trimmed();
}

// Units in the hwmon ABI, from Documentation/hwmon/sysfs-interface.rst.
//
// GOTCHA, and the one worth writing down: **fan speed is already in RPM.**
// Every other channel here is scaled — temperatures are millidegrees, volts
// are millivolts, power is microwatts, current is milliamps — so "divide by
// a thousand" reads like the rule and is wrong for exactly one family. A fan
// at 1200 RPM divided by 1000 becomes a plausible-looking 1.2, which is
// precisely the kind of wrong number this project's graphs are supposed not
// to draw.
double scaleForKind(Kind kind)
{
    switch (kind) {
    case Kind::Temperature: // millidegrees C
    case Kind::Voltage:     // millivolts
    case Kind::Current:     // milliamps
        return 1000.0;
    case Kind::Power: // microwatts
        return 1000000.0;
    case Kind::Fan: // RPM, unscaled
    case Kind::FanDuty: // not an hwmon family; listed so the switch is total
        return 1.0;
    }
    return 1.0;
}

// sysfs prefix -> kind. The digits in the pattern below matter: `intrusion0_*`
// also starts with "in", and only requiring a digit straight after the prefix
// keeps it from being read as voltage channel "trusion".
Kind kindForPrefix(const QString &prefix)
{
    if (prefix == QLatin1String("temp")) {
        return Kind::Temperature;
    }
    if (prefix == QLatin1String("fan")) {
        return Kind::Fan;
    }
    if (prefix == QLatin1String("power")) {
        return Kind::Power;
    }
    if (prefix == QLatin1String("curr")) {
        return Kind::Current;
    }
    return Kind::Voltage; // "in"
}

// Upper bound above which a published limit is not a limit but a sentinel.
//
// Measured, not guessed: this box's NVMe controller reports `temp2_max` and
// `temp3_max` as 65261850 millidegrees. That is 65535.0 K exactly — 0xFFFF,
// the NVMe spec's "this threshold is not implemented" value — and rendering it
// verbatim would put "65261.9 °C" on screen next to a 44 °C reading. Chips
// publish nonsense in unimplemented registers far more often than they omit
// the attribute, so an implausible limit has to be dropped as firmly as a
// missing one.
double implausibleAbove(Kind kind)
{
    switch (kind) {
    case Kind::Temperature:
        return 200.0; // °C; silicon that hot has stopped being silicon
    case Kind::Fan:
        return 100000.0; // RPM
    case Kind::Voltage:
        return 1000.0; // V
    case Kind::Power:
        return 10000.0; // W
    case Kind::Current:
        return 10000.0; // A
    case Kind::FanDuty:
        return 100.0; // %
    }
    return 0.0;
}

QString describeKind(Kind kind)
{
    switch (kind) {
    case Kind::Temperature:
        return QCoreApplication::translate("SensorProvider", "Temperature");
    case Kind::Fan:
        return QCoreApplication::translate("SensorProvider", "Fan");
    case Kind::Voltage:
        return QCoreApplication::translate("SensorProvider", "Voltage");
    case Kind::Power:
        return QCoreApplication::translate("SensorProvider", "Power");
    case Kind::Current:
        return QCoreApplication::translate("SensorProvider", "Current");
    case Kind::FanDuty:
        return QCoreApplication::translate("SensorProvider", "Fan");
    }
    return {};
}

// Curated chip names. hwmon reports driver names, which are precise and
// meaningless to most people: "k10temp" is the CPU, "spd5118" is a DDR5 DIMM.
// Anything not listed keeps its raw driver name, which is the right answer for
// a chip nobody here has seen — better an honest "nct6687" than a guess.
struct ChipName {
    const char *driver;
    const char *display;
};

constexpr ChipName kChipNames[] = {
    {"k10temp", QT_TRANSLATE_NOOP("SensorProvider", "CPU")},
    {"zenpower", QT_TRANSLATE_NOOP("SensorProvider", "CPU")},
    {"coretemp", QT_TRANSLATE_NOOP("SensorProvider", "CPU")},
    {"cpu_thermal", QT_TRANSLATE_NOOP("SensorProvider", "CPU")},
    {"amdgpu", QT_TRANSLATE_NOOP("SensorProvider", "GPU")},
    {"radeon", QT_TRANSLATE_NOOP("SensorProvider", "GPU")},
    {"i915", QT_TRANSLATE_NOOP("SensorProvider", "GPU")},
    {"xe", QT_TRANSLATE_NOOP("SensorProvider", "GPU")},
    {"nouveau", QT_TRANSLATE_NOOP("SensorProvider", "GPU")},
    {"nvme", QT_TRANSLATE_NOOP("SensorProvider", "NVMe drive")},
    {"drivetemp", QT_TRANSLATE_NOOP("SensorProvider", "Drive")},
    {"spd5118", QT_TRANSLATE_NOOP("SensorProvider", "Memory module")},
    {"jc42", QT_TRANSLATE_NOOP("SensorProvider", "Memory module")},
    {"acpitz", QT_TRANSLATE_NOOP("SensorProvider", "ACPI thermal zone")},
    {"iwlwifi", QT_TRANSLATE_NOOP("SensorProvider", "Wi-Fi adapter")},
    {"mt7921", QT_TRANSLATE_NOOP("SensorProvider", "Wi-Fi adapter")},
};

// Driver names that the GPU pages already surface a temperature for. Listing
// them again here is deliberate — the sensors page is where someone goes to
// see every temperature at once, and silently omitting the hottest chip in
// the machine would be the surprising behaviour — but the duplication is
// labelled so it reads as a decision rather than an oversight.
bool isGpuDriver(const QString &name)
{
    static const QStringList kGpuDrivers{
        QStringLiteral("amdgpu"), QStringLiteral("radeon"),
        QStringLiteral("i915"),   QStringLiteral("xe"),
        QStringLiteral("nouveau"),
    };
    return kGpuDrivers.contains(name);
}

QString curatedName(const QString &driver)
{
    for (const ChipName &entry : kChipNames) {
        // Several drivers register as "<name>_<instance>" — iwlwifi shows up
        // as "iwlwifi_1" — so a prefix match is what actually hits.
        const QString candidate = QString::fromLatin1(entry.driver);
        if (driver == candidate || driver.startsWith(candidate + QLatin1Char('_'))) {
            return QCoreApplication::translate("SensorProvider", entry.display);
        }
    }
    return driver;
}

// hwmon directories sort as strings by default, which puts hwmon10 before
// hwmon2. The numeric order is the order the kernel registered them in, and
// is what anyone cross-checking against `sensors` will see.
int hwmonIndex(const QString &dirName)
{
    return QStringView(dirName).mid(5).toInt();
}

} // namespace

HwmonSensorProvider::HwmonSensorProvider(QString root)
    : m_root(std::move(root))
{
    enumerateChips();
    assignDisplayNames();
}

bool HwmonSensorProvider::isAvailable() const
{
    return !m_chips.isEmpty();
}

void HwmonSensorProvider::enumerateChips()
{
    const QDir hwmonRoot(m_root + QStringLiteral("/sys/class/hwmon"));
    QStringList entries =
        hwmonRoot.entryList(QStringList{QStringLiteral("hwmon*")},
                            QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(entries.begin(), entries.end(),
              [](const QString &a, const QString &b) {
                  return hwmonIndex(a) < hwmonIndex(b);
              });

    // Matches the reading attribute of any channel family. `_average` is
    // accepted alongside `_input` because some AMD cards publish only the
    // average power; scanning for `_input` alone would have left those chips
    // with no power channel at all, and the fallback inside sample() would
    // then have been unreachable code.
    static const QRegularExpression channelPattern(
        QStringLiteral("^(temp|fan|in|power|curr)(\\d+)_(input|average)$"));

    for (const QString &entry : entries) {
        const QString path = hwmonRoot.filePath(entry);
        const QString name = readSysfsLine(path + QStringLiteral("/name"));
        if (name.isEmpty()) {
            // A hwmon node without a name cannot be identified or labelled,
            // and in practice does not occur; skipping beats inventing one.
            continue;
        }

        SensorChipInfo chip;
        chip.id = entry;
        chip.name = name;
        chip.deviceId =
            QFileInfo(QFileInfo(path + QStringLiteral("/device")).symLinkTarget())
                .fileName();
        if (isGpuDriver(name)) {
            chip.alsoShownIn =
                QCoreApplication::translate("SensorProvider", "the GPU page");
        }

        const QStringList files = QDir(path).entryList(QDir::Files | QDir::System);
        for (const QString &file : files) {
            const QRegularExpressionMatch match = channelPattern.match(file);
            if (!match.hasMatch()) {
                continue;
            }
            SensorChannelInfo channel;
            channel.kind = kindForPrefix(match.captured(1));
            channel.id = match.captured(1) + match.captured(2);
            // A chip publishing both `_input` and `_average` for one channel
            // matches twice. Either match produces an identical entry — which
            // attribute is read is decided in sample(), which always tries
            // `_input` first — so dropping the second is enough. (Do not read
            // the "keep the first" as a preference: entryList sorts by name,
            // so `_average` is actually the one that arrives first.)
            const auto duplicate =
                std::find_if(chip.channels.cbegin(), chip.channels.cend(),
                             [&channel](const SensorChannelInfo &existing) {
                                 return existing.id == channel.id;
                             });
            if (duplicate != chip.channels.cend()) {
                continue;
            }

            channel.label = readSysfsLine(path + QLatin1Char('/') + channel.id
                                          + QStringLiteral("_label"));
            if (channel.label.isEmpty()) {
                // Most chips outside the enthusiast motherboard world publish
                // no labels at all — spd5118 and iwlwifi on this box — so a
                // synthesized "Temperature 1" is the common case, not the
                // fallback. Never render the bare sysfs id at a user.
                channel.label = QStringLiteral("%1 %2")
                                    .arg(describeKind(channel.kind),
                                         match.captured(2));
            }

            const double scale = scaleForKind(channel.kind);
            const double ceiling = implausibleAbove(channel.kind);
            const QString base = path + QLatin1Char('/') + channel.id;
            bool ok = false;
            const double crit =
                readSysfsLine(base + QStringLiteral("_crit")).toDouble(&ok) / scale;
            if (ok && crit > 0.0 && crit < ceiling) {
                channel.criticalLimit = crit;
            }
            const double high =
                readSysfsLine(base + QStringLiteral("_max")).toDouble(&ok) / scale;
            if (ok && high > 0.0 && high < ceiling) {
                channel.highLimit = high;
            }

            chip.channels.append(channel);
        }

        if (chip.channels.isEmpty()) {
            // Plenty of hwmon nodes carry no readable channel at all — a
            // hidpp battery registers one and exposes nothing this page can
            // plot. An empty group box would be pure noise.
            continue;
        }

        std::sort(chip.channels.begin(), chip.channels.end(),
                  [](const SensorChannelInfo &a, const SensorChannelInfo &b) {
                      if (a.kind != b.kind) {
                          return a.kind < b.kind;
                      }
                      return a.id.localeAwareCompare(b.id) < 0;
                  });

        m_chips.append(chip);
        m_chipPaths.append(path);
    }
}

void HwmonSensorProvider::assignDisplayNames()
{
    // The curated name is not unique — two DIMMs both map to "Memory module",
    // and a machine with two NVMe drives gets two "NVMe drive"s. Number them
    // only when there is genuine ambiguity, so a machine with one CPU chip
    // says "CPU" rather than the faintly absurd "CPU 1".
    QHash<QString, int> counts;
    for (SensorChipInfo &chip : m_chips) {
        chip.displayName = curatedName(chip.name);
        counts[chip.displayName] += 1;
    }

    QHash<QString, int> seen;
    for (SensorChipInfo &chip : m_chips) {
        if (counts.value(chip.displayName) < 2) {
            continue;
        }
        const int index = (seen[chip.displayName] += 1);
        chip.displayName = QStringLiteral("%1 %2").arg(chip.displayName).arg(index);
    }
}

QVector<SensorChipSnapshot> HwmonSensorProvider::sample()
{
    // Measured on this box: a full pass costs ~4.5 ms, and it is not evenly
    // spread. Subtracting process-spawn overhead, k10temp, amdgpu and iwlwifi
    // answer in 10-80 us, the NVMe controller takes ~400 us per channel, and
    // each spd5118 DIMM takes ~1.3 ms — because these are not memory reads at
    // all. An hwmon attribute read issues the underlying transaction: an i2c
    // exchange on the SMBus for the DIMMs, an admin command for NVMe. This is
    // the most expensive per-byte read anywhere in the app.
    //
    // Temperatures do not move fast enough to justify paying that on every
    // tick. The floor below is deliberately just under one second: at the
    // default 1 s speed every tick still gets a fresh reading, so the graph
    // has no staircase, while the 0.5 s speed samples every other tick and
    // the cost stays flat instead of doubling. Same reasoning as the disk
    // provider's mount-refresh cap, and the same reason.
    if (m_sinceLastRead.isValid()
        && m_sinceLastRead.elapsed() < kMinSampleIntervalMsec) {
        return m_lastSnapshots;
    }
    m_sinceLastRead.restart();

    QVector<SensorChipSnapshot> snapshots;
    snapshots.reserve(m_chips.size());

    for (int i = 0; i < m_chips.size(); ++i) {
        const SensorChipInfo &chip = m_chips.at(i);
        const QString &path = m_chipPaths.at(i);

        SensorChipSnapshot snapshot;
        snapshot.chipId = chip.id;
        snapshot.readings.reserve(chip.channels.size());

        for (const SensorChannelInfo &channel : chip.channels) {
            SensorReading reading;
            reading.channelId = channel.id;

            bool ok = false;
            const QString base = path + QLatin1Char('/') + channel.id;
            double raw = readSysfsLine(base + QStringLiteral("_input")).toDouble(&ok);
            if (!ok && channel.kind == Kind::Power) {
                // Some chips publish an average instead of an instantaneous
                // reading; the GPU backend has always taken that fallback.
                raw = readSysfsLine(base + QStringLiteral("_average")).toDouble(&ok);
            }
            if (ok) {
                reading.value = raw / scaleForKind(channel.kind);
            }
            // A channel that vanishes or starts erroring keeps its -1 and is
            // rendered as an em dash. Sensors do disappear at runtime: a
            // driver unbind takes the whole directory with it.
            snapshot.readings.append(reading);
        }

        snapshots.append(snapshot);
    }

    m_lastSnapshots = snapshots;
    return snapshots;
}
