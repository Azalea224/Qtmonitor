#include "hardwareinfo.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>

namespace {

QString readTrimmed(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

QString firstCpuinfoValue(const QString &key)
{
    QFile file(QStringLiteral("/proc/cpuinfo"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QTextStream in(&file);
    QString line;
    const QString prefix = key + QLatin1Char('\t');
    while (!(line = in.readLine()).isNull()) {
        if (line.startsWith(prefix) || line.startsWith(key + QLatin1Char(' '))) {
            const int colon = line.indexOf(QLatin1Char(':'));
            if (colon >= 0) {
                return line.mid(colon + 1).trimmed();
            }
        }
    }
    return {};
}

QString formatKib(quint64 kib)
{
    if (kib >= 1024 && kib % 1024 == 0) {
        return QStringLiteral("%1 MiB").arg(kib / 1024);
    }
    return QStringLiteral("%1 KiB").arg(kib);
}

QVector<QPair<QString, QString>> cachesFromSysfs()
{
    // cpu0's cache hierarchy: index dirs with level/type/size files.
    // L1/L2 are per-core, L3 is shared — reported as read (like lscpu's
    // per-instance values).
    const QDir cacheDir(QStringLiteral("/sys/devices/system/cpu/cpu0/cache"));
    QVector<QPair<QString, QString>> caches;
    for (const QString &index : cacheDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = cacheDir.filePath(index);
        const QString level = readTrimmed(base + QStringLiteral("/level"));
        const QString type = readTrimmed(base + QStringLiteral("/type"));
        const QString sizeStr = readTrimmed(base + QStringLiteral("/size"));
        if (level.isEmpty() || sizeStr.isEmpty()) {
            continue;
        }
        // size looks like "32K" or "1M"
        bool ok = false;
        quint64 value = sizeStr.left(sizeStr.size() - 1).toULongLong(&ok);
        if (!ok) {
            continue;
        }
        if (sizeStr.endsWith(QLatin1Char('K'))) {
            value *= 1;
        } else if (sizeStr.endsWith(QLatin1Char('M'))) {
            value *= 1024;
        }
        QString label = QStringLiteral("L%1").arg(level);
        if (type == QLatin1String("Data")) {
            label += QLatin1Char('d');
        } else if (type == QLatin1String("Instruction")) {
            label += QLatin1Char('i');
        }
        caches.append({label, formatKib(value)});
    }
    // Stable L1d, L1i, L2, L3 ordering
    std::sort(caches.begin(), caches.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    return caches;
}

QStringList instructionSetsFromCpuinfo()
{
    const QString flags = firstCpuinfoValue(QStringLiteral("flags"));
    if (flags.isEmpty()) {
        return {};
    }
    const QStringList flagList = flags.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QSet<QString> present(flagList.begin(), flagList.end());

    // Curated highlights in a sensible display order, not the full flags dump
    static const struct { const char *flag; const char *label; } kKnown[] = {
        {"sse4_1", "SSE4.1"}, {"sse4_2", "SSE4.2"},
        {"avx", "AVX"}, {"avx2", "AVX2"}, {"avx512f", "AVX-512"},
        {"fma", "FMA"}, {"aes", "AES-NI"}, {"sha_ni", "SHA-NI"},
        {"bmi1", "BMI1"}, {"bmi2", "BMI2"},
        {"vmx", "VT-x"}, {"svm", "AMD-V"},
    };
    QStringList found;
    for (const auto &entry : kKnown) {
        if (present.contains(QLatin1String(entry.flag))) {
            found.append(QLatin1String(entry.label));
        }
    }
    return found;
}

} // namespace

CpuStaticInfo loadCpuStaticInfo()
{
    CpuStaticInfo info;
    info.modelName = firstCpuinfoValue(QStringLiteral("model name"));

    const QString cpufreq =
        QStringLiteral("/sys/devices/system/cpu/cpu0/cpufreq");
    bool ok = false;
    const quint64 maxKhz = readTrimmed(cpufreq + QStringLiteral("/cpuinfo_max_freq"))
                               .toULongLong(&ok);
    if (ok) {
        info.maxFreqGhz = maxKhz / 1.0e6;
    }
    info.driver = readTrimmed(cpufreq + QStringLiteral("/scaling_driver"));
    info.governor = readTrimmed(cpufreq + QStringLiteral("/scaling_governor"));
    info.energyPreference =
        readTrimmed(cpufreq + QStringLiteral("/energy_performance_preference"));

    info.caches = cachesFromSysfs();
    info.instructionSets = instructionSetsFromCpuinfo();
    return info;
}

CpuDynamicInfo loadCpuDynamicInfo()
{
    const QString cpufreq =
        QStringLiteral("/sys/devices/system/cpu/cpu0/cpufreq");
    CpuDynamicInfo info;
    info.governor = readTrimmed(cpufreq + QStringLiteral("/scaling_governor"));
    info.energyPreference =
        readTrimmed(cpufreq + QStringLiteral("/energy_performance_preference"));
    return info;
}

namespace {

// SMBIOS often leaves the Manufacturer string "Unknown" while still
// reporting the JEDEC module manufacturer ID, which udev exposes as
// "Bank 5, Hex 0xCD". Decode the common consumer vendors; anything else
// falls back to showing the raw ID rather than guessing.
QString jedecManufacturer(const QString &rawId)
{
    static const struct { int bank; int hex; const char *name; } kVendors[] = {
        {1, 0x2C, "Micron"},     {1, 0x98, "Kingston"},
        {1, 0xAD, "SK hynix"},   {1, 0xCE, "Samsung"},
        {3, 0x9E, "Corsair"},    {4, 0xCB, "A-DATA"},
        {5, 0xCD, "G.Skill"},
    };

    // "Bank 5, Hex 0xCD"
    static const QRegularExpression re(
        QStringLiteral("Bank\\s+(\\d+),\\s*Hex\\s+0x([0-9A-Fa-f]+)"));
    const QRegularExpressionMatch match = re.match(rawId);
    if (!match.hasMatch()) {
        return {};
    }
    const int bank = match.captured(1).toInt();
    const int hex = match.captured(2).toInt(nullptr, 16);
    for (const auto &vendor : kVendors) {
        if (vendor.bank == bank && vendor.hex == hex) {
            return QLatin1String(vendor.name);
        }
    }
    return rawId; // honest fallback: show the ID we could not map
}

bool isMeaningful(const QString &value)
{
    return !value.isEmpty()
        && value != QLatin1String("Unknown")
        && value != QLatin1String("Not Specified")
        && value != QLatin1String("None");
}

// Reads DMI memory-device properties from the udev database. udev runs its
// dmi-memory-id builtin at boot and stores the result, so this works as an
// unprivileged user — unlike dmidecode, which needs root for
// /sys/firmware/dmi/tables. Querying the dmi/id device directly is ~100x
// cheaper than dumping the whole database.
bool loadMemoryInfoFromUdev(MemoryStaticInfo &info)
{
    const QString udevadm = QStandardPaths::findExecutable(QStringLiteral("udevadm"));
    if (udevadm.isEmpty()) {
        return false;
    }

    QProcess process;
    process.start(udevadm, {QStringLiteral("info"), QStringLiteral("-p"),
                            QStringLiteral("/sys/devices/virtual/dmi/id")});
    if (!process.waitForFinished(3000) || process.exitCode() != 0) {
        return false;
    }

    // Lines look like: "E: MEMORY_DEVICE_1_SIZE=17179869184"
    static const QRegularExpression re(
        QStringLiteral("^E:\\s*MEMORY_DEVICE_(\\d+)_(\\w+)=(.*)$"));
    QHash<int, QHash<QString, QString>> devices;
    const QStringList lines =
        QString::fromUtf8(process.readAllStandardOutput()).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = re.match(line);
        if (match.hasMatch()) {
            devices[match.captured(1).toInt()].insert(match.captured(2),
                                                      match.captured(3).trimmed());
        }
    }
    if (devices.isEmpty()) {
        return false;
    }

    quint64 totalBytes = 0;
    int count = 0;
    for (const auto &fields : devices) {
        // Unpopulated slots report PRESENT=0 and carry no SIZE
        const quint64 sizeBytes = fields.value(QStringLiteral("SIZE")).toULongLong();
        if (sizeBytes == 0) {
            continue;
        }
        ++count;
        totalBytes += sizeBytes;

        const QString type = fields.value(QStringLiteral("TYPE"));
        if (isMeaningful(type)) {
            info.type = type;
        }
        const double configured =
            fields.value(QStringLiteral("CONFIGURED_SPEED_MTS")).toDouble();
        if (configured > 0) {
            info.configuredSpeedMTs = configured;
        }
        const double rated = fields.value(QStringLiteral("SPEED_MTS")).toDouble();
        if (rated > 0) {
            info.ratedSpeedMTs = rated;
        }
        const QString part = fields.value(QStringLiteral("PART_NUMBER"));
        if (isMeaningful(part)) {
            info.partNumber = part;
        }
        const int rank = fields.value(QStringLiteral("RANK")).toInt();
        if (rank > 0) {
            info.ranks = rank;
        }

        const QString maker = fields.value(QStringLiteral("MANUFACTURER"));
        if (isMeaningful(maker)) {
            info.manufacturer = maker;
        } else {
            const QString decoded =
                jedecManufacturer(fields.value(QStringLiteral("MODULE_MANUFACTURER_ID")));
            if (!decoded.isEmpty()) {
                info.manufacturer = decoded;
            }
        }
    }

    if (count == 0) {
        return false;
    }
    info.available = true;
    info.stickCount = count;
    info.perStickBytes = totalBytes / count;
    return true;
}

} // namespace

MemoryStaticInfo loadMemoryStaticInfo()
{
    MemoryStaticInfo info;

    // Preferred: udev database — works without root.
    if (loadMemoryInfoFromUdev(info)) {
        return info;
    }

    // Fallback for systems whose udev did not populate DMI properties
    // (older systemd). Needs root, so it usually fails silently.
    const QString dmidecode = QStandardPaths::findExecutable(QStringLiteral("dmidecode"));
    if (dmidecode.isEmpty()) {
        return info;
    }

    QProcess process;
    process.start(dmidecode, {QStringLiteral("-t"), QStringLiteral("17")});
    if (!process.waitForFinished(3000) || process.exitCode() != 0) {
        return info;
    }

    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    const QStringList blocks = output.split(QStringLiteral("Memory Device"), Qt::SkipEmptyParts);

    quint64 totalBytes = 0;
    int count = 0;
    for (const QString &block : blocks) {
        quint64 sizeBytes = 0;
        const QStringList lines = block.split(QLatin1Char('\n'));
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.startsWith(QLatin1String("Size:"))) {
                const QString value = line.mid(5).trimmed();
                if (value.startsWith(QLatin1String("No Module"))) {
                    break;
                }
                const QStringList parts = value.split(QLatin1Char(' '));
                if (parts.size() >= 2) {
                    const double amount = parts.at(0).toDouble();
                    if (parts.at(1) == QLatin1String("GB")) {
                        sizeBytes = static_cast<quint64>(amount * 1073741824.0);
                    } else if (parts.at(1) == QLatin1String("MB")) {
                        sizeBytes = static_cast<quint64>(amount * 1048576.0);
                    }
                }
            } else if (line.startsWith(QLatin1String("Configured Memory Speed:"))) {
                info.configuredSpeedMTs =
                    line.mid(24).trimmed().split(QLatin1Char(' ')).first().toDouble();
            } else if (line.startsWith(QLatin1String("Type:"))) {
                const QString type = line.mid(5).trimmed();
                if (isMeaningful(type)) {
                    info.type = type;
                }
            } else if (line.startsWith(QLatin1String("Part Number:"))) {
                const QString part = line.mid(12).trimmed();
                if (isMeaningful(part)) {
                    info.partNumber = part;
                }
            } else if (line.startsWith(QLatin1String("Manufacturer:"))) {
                const QString maker = line.mid(13).trimmed();
                if (isMeaningful(maker)) {
                    info.manufacturer = maker;
                }
            }
        }
        if (sizeBytes > 0) {
            ++count;
            totalBytes += sizeBytes;
        }
    }

    if (count > 0) {
        info.available = true;
        info.stickCount = count;
        info.perStickBytes = totalBytes / count;
    }
    return info;
}
