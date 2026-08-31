#include "diskprovider.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>
#include <utility>

#include <sys/statvfs.h>

namespace {

// /proc/diskstats reports sectors in fixed 512-byte units regardless of the
// device's own logical block size — see Documentation/admin-guide/iostats.rst.
// Reading queue/hw_sector_size and multiplying by that is a classic way to get
// this wrong by a factor of 8 on a 4K-sector drive.
constexpr quint64 kDiskstatsSectorBytes = 512;

QString readSysfsLine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readLine()).trimmed();
}

// Kernel names that are never worth a sidebar row: the legacy ramdisks exist
// in a fixed set of 16 and are always idle, and an unbacked loop device has
// no size.
bool isUninterestingBlockDevice(const QString &name, quint64 sizeBytes)
{
    if (sizeBytes == 0) {
        return true;
    }
    return name.startsWith(QLatin1String("ram"));
}

QString describeKind(const QString &name, bool rotational, bool removable)
{
    if (name.startsWith(QLatin1String("zram"))) {
        return QCoreApplication::translate("DiskProvider", "Compressed RAM");
    }
    if (removable) {
        return rotational
            ? QCoreApplication::translate("DiskProvider", "Removable disk")
            : QCoreApplication::translate("DiskProvider", "Removable drive");
    }
    return rotational ? QCoreApplication::translate("DiskProvider", "HDD")
                      : QCoreApplication::translate("DiskProvider", "SSD");
}

// Maps a partition's kernel name to the whole disk it lives on. /sys/class/block
// entries are symlinks into the device tree, where a partition sits inside its
// disk's directory, so the parent component is the answer. A device with no
// "partition" attribute already is a whole disk.
QString diskForBlockDevice(const QString &root, const QString &kernelName)
{
    const QString base = root + QStringLiteral("/sys/class/block/") + kernelName;
    if (!QFile::exists(base)) {
        return {};
    }
    if (!QFile::exists(base + QStringLiteral("/partition"))) {
        return kernelName;
    }
    const QString target = QFileInfo(base).symLinkTarget();
    if (target.isEmpty()) {
        return {};
    }
    return QFileInfo(target).dir().dirName();
}

} // namespace

ProcfsDiskProvider::ProcfsDiskProvider(QString root)
    : m_root(std::move(root))
{
    enumerateDevices();
}

bool ProcfsDiskProvider::isAvailable() const
{
    return QFile::exists(m_root + QStringLiteral("/proc/diskstats"))
        && !m_devices.isEmpty();
}

void ProcfsDiskProvider::enumerateDevices()
{
    const QDir blockDir(m_root + QStringLiteral("/sys/block"));
    const QStringList names =
        blockDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    for (const QString &name : names) {
        const QString base = blockDir.absoluteFilePath(name);

        DiskDeviceInfo device;
        device.id = name;
        // sysfs reports size in 512-byte sectors here too.
        device.sizeBytes = readSysfsLine(base + QStringLiteral("/size")).toULongLong()
            * kDiskstatsSectorBytes;
        if (isUninterestingBlockDevice(name, device.sizeBytes)) {
            continue;
        }

        device.rotational =
            readSysfsLine(base + QStringLiteral("/queue/rotational")) == QLatin1String("1");
        device.removable =
            readSysfsLine(base + QStringLiteral("/removable")) == QLatin1String("1");
        device.kind = describeKind(name, device.rotational, device.removable);

        // Vendor and model come back space-padded out of sysfs. "ATA" is the
        // transport the SCSI layer reports for every SATA disk, not a
        // manufacturer, and prefixing it onto the model helps nobody.
        QString vendor =
            readSysfsLine(base + QStringLiteral("/device/vendor")).simplified();
        if (vendor.compare(QLatin1String("ATA"), Qt::CaseInsensitive) == 0) {
            vendor.clear();
        }
        const QString model =
            readSysfsLine(base + QStringLiteral("/device/model")).simplified();
        QString label = vendor.isEmpty() ? model
                                         : QStringLiteral("%1 %2").arg(vendor, model);
        label = label.simplified();
        device.name = label.isEmpty() ? name : label;

        // The PCI/USB address, when the device hangs off a real bus. Virtual
        // devices (zram, device-mapper) have no such link.
        const QString deviceLink =
            QFileInfo(base + QStringLiteral("/device")).symLinkTarget();
        if (!deviceLink.isEmpty() && !deviceLink.contains(QLatin1String("/virtual/"))) {
            device.busPath = QFileInfo(deviceLink).fileName();
        }

        m_devices.append(device);
    }
}

void ProcfsDiskProvider::refreshMounts()
{
    m_mounts.clear();

    QFile file(m_root + QStringLiteral("/proc/self/mounts"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    // Every mount point found for a given source device, so one filesystem
    // becomes one row however many places it is mounted.
    QHash<QString, DiskMountInfo> byDevice;
    QHash<QString, QString> diskOfDevice;

    QTextStream in(&file);
    // Do NOT loop on atEnd(): procfs files report st_size 0, which makes
    // atEnd() return true immediately. Loop until readLine() yields null.
    QString line;
    while (!(line = in.readLine()).isNull()) {
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 3) {
            continue;
        }
        const QString source = parts.at(0);
        // Only real block devices: a fuse, network or pseudo filesystem cannot
        // be attributed to a disk, and statvfs() on a dead NFS server hangs.
        if (!source.startsWith(QLatin1String("/dev/"))) {
            continue;
        }

        // Paths in mounts are escaped octal for space, tab, newline and
        // backslash.
        QString mountPoint = parts.at(1);
        mountPoint.replace(QLatin1String("\\040"), QLatin1String(" "));
        mountPoint.replace(QLatin1String("\\011"), QLatin1String("\t"));
        mountPoint.replace(QLatin1String("\\012"), QLatin1String("\n"));
        mountPoint.replace(QLatin1String("\\134"), QLatin1String("\\"));

        // /dev/mapper/* and /dev/disk/by-*/* are symlinks, and it is the
        // resolved kernel name that /sys/class/block is keyed on.
        const QFileInfo sourceInfo(source);
        const QString canonical = sourceInfo.canonicalFilePath();
        const QString kernelName = canonical.isEmpty()
            ? sourceInfo.fileName()
            : QFileInfo(canonical).fileName();
        const QString diskId = diskForBlockDevice(m_root, kernelName);
        if (diskId.isEmpty()) {
            continue;
        }

        const auto existing = byDevice.find(source);
        if (existing != byDevice.end()) {
            // Same filesystem, another path into it: record the path and skip
            // the statvfs, which would only return the same numbers again.
            if (!existing->mountPoints.contains(mountPoint)) {
                existing->mountPoints.append(mountPoint);
            }
            continue;
        }

        struct statvfs stats;
        if (statvfs(QFile::encodeName(mountPoint).constData(), &stats) != 0) {
            continue;
        }

        DiskMountInfo mount;
        mount.device = source;
        mount.mountPoints.append(mountPoint);
        mount.filesystem = parts.at(2);
        mount.totalBytes = static_cast<quint64>(stats.f_blocks) * stats.f_frsize;
        // f_bfree counts blocks free to root, f_bavail those free to everyone
        // else. "Used" must be measured against the total, so it is the
        // former; the difference is the reserve, which is genuinely occupied
        // from a normal user's point of view but not lost.
        mount.usedBytes =
            static_cast<quint64>(stats.f_blocks - stats.f_bfree) * stats.f_frsize;
        if (mount.totalBytes == 0) {
            continue;
        }

        byDevice.insert(source, mount);
        diskOfDevice.insert(source, diskId);
    }

    for (auto it = byDevice.begin(); it != byDevice.end(); ++it) {
        // Shortest path first: the one a reader recognises as the filesystem's
        // real home, "/" ahead of "/var/cache".
        std::sort(it->mountPoints.begin(), it->mountPoints.end(),
                  [](const QString &a, const QString &b) {
                      return a.size() != b.size() ? a.size() < b.size() : a < b;
                  });
        m_mounts[diskOfDevice.value(it.key())].append(*it);
    }

    // Same rule between filesystems, so /boot never sorts above /.
    for (auto it = m_mounts.begin(); it != m_mounts.end(); ++it) {
        std::sort(it->begin(), it->end(),
                  [](const DiskMountInfo &a, const DiskMountInfo &b) {
                      const QString &x = a.mountPoints.first();
                      const QString &y = b.mountPoints.first();
                      return x.size() != y.size() ? x.size() < y.size() : x < y;
                  });
    }
}

QVector<DiskSnapshot> ProcfsDiskProvider::sample()
{
    // First call has no previous reading to subtract from, so it establishes
    // the baseline and reports zero rather than inventing rates from boot-time
    // totals.
    const bool first = !m_elapsed.isValid();
    const double elapsedMsec = first ? 0.0 : static_cast<double>(m_elapsed.restart());
    if (first) {
        m_elapsed.start();
    }
    return sampleWithElapsed(elapsedMsec);
}

QVector<DiskSnapshot> ProcfsDiskProvider::sampleWithElapsed(double elapsedMsec)
{
    if (!m_sinceMountRefresh.isValid()
        || m_sinceMountRefresh.elapsed() >= kMountRefreshMsec) {
        refreshMounts();
        m_sinceMountRefresh.restart();
    }

    QHash<QString, Counters> current;

    QFile file(m_root + QStringLiteral("/proc/diskstats"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        QString line;
        while (!(line = in.readLine()).isNull()) {
            const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            // major minor name, then at least the 11 classic statistics.
            if (parts.size() < 14) {
                continue;
            }
            Counters counters;
            counters.sectorsRead = parts.at(5).toULongLong();
            counters.sectorsWritten = parts.at(9).toULongLong();
            counters.inFlight = parts.at(11).toInt();
            counters.ioTicksMsec = parts.at(12).toULongLong();
            counters.weightedIoTicksMsec = parts.at(13).toULongLong();
            counters.valid = true;
            current.insert(parts.at(2), counters);
        }
    }

    QVector<DiskSnapshot> snapshots;
    snapshots.reserve(m_devices.size());

    for (const DiskDeviceInfo &device : m_devices) {
        DiskSnapshot snapshot;
        snapshot.id = device.id;
        snapshot.mounts = m_mounts.value(device.id);

        const auto now = current.constFind(device.id);
        if (now != current.cend()) {
            snapshot.inFlight = now->inFlight;

            const Counters previous = m_previous.value(device.id);
            // Counters are monotonic while the device exists; a smaller value
            // means it went away and came back, so skip that interval rather
            // than plotting a negative rate as a huge positive one.
            if (previous.valid && elapsedMsec > 0.0
                && now->sectorsRead >= previous.sectorsRead
                && now->sectorsWritten >= previous.sectorsWritten
                && now->ioTicksMsec >= previous.ioTicksMsec) {
                const double seconds = elapsedMsec / 1000.0;
                snapshot.readBytesPerSec =
                    (now->sectorsRead - previous.sectorsRead) * kDiskstatsSectorBytes
                    / seconds;
                snapshot.writeBytesPerSec =
                    (now->sectorsWritten - previous.sectorsWritten) * kDiskstatsSectorBytes
                    / seconds;
                // io_ticks accrues at most one millisecond per elapsed
                // millisecond, but rounding at both ends can push the ratio a
                // hair over 1.
                snapshot.activePercent = qBound(
                    0.0, 100.0 * (now->ioTicksMsec - previous.ioTicksMsec) / elapsedMsec,
                    100.0);
                if (now->weightedIoTicksMsec >= previous.weightedIoTicksMsec) {
                    snapshot.avgQueueLength =
                        (now->weightedIoTicksMsec - previous.weightedIoTicksMsec)
                        / elapsedMsec;
                }
            }
        }

        snapshots.append(snapshot);
    }

    m_previous = current;
    return snapshots;
}
