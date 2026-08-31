#include "drmgpuprovider.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <dirent.h>
#include <unistd.h>

#include <utility>

#include "optionaltools.h"

namespace {

// Reads a whole /proc or /sys file. readAll() rather than a QTextStream line
// loop on purpose: procfs and sysfs report st_size 0, which makes atEnd()
// true immediately, while readAll() keeps reading to EOF.
QByteArray readSysFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll().trimmed();
}

// Drivers known to implement DRM fdinfo engine statistics. For these,
// a missing drm-engine-* line means the engine was idle; for anything else
// it means we cannot tell, and the UI shows an em dash instead of a
// misleading 0%.
bool engineStatsExpected(const QString &driver)
{
    static const QStringList kEngineStatsDrivers = {
        QStringLiteral("amdgpu"),   QStringLiteral("i915"),
        QStringLiteral("xe"),       QStringLiteral("msm"),
        QStringLiteral("panfrost"), QStringLiteral("panthor"),
        QStringLiteral("v3d"),      QStringLiteral("lima"),
        QStringLiteral("etnaviv"),
    };
    return kEngineStatsDrivers.contains(driver);
}

// DRM nodes that are not really GPUs: the firmware framebuffer shim and the
// virtual test device. Listing them would put dead rows in the sidebar.
bool isNonGpuDriver(const QString &driver)
{
    return driver.isEmpty() || driver == QStringLiteral("simpledrm")
        || driver == QStringLiteral("vkms");
}

// Video engines across the drivers we support: amdgpu (dec, enc, enc_1,
// jpeg, vpe), i915 (video, video-enhance) and xe (vcs, vecs). Everything
// else — gfx, compute, render, rcs, ccs, dma, bcs, copy — counts as general
// GPU work, which is the split the two process columns present.
bool isVideoEngine(const QByteArray &engine)
{
    static const char *kVideoPrefixes[] = {"dec", "enc", "jpeg", "vpe",
                                           "video", "vcs", "vecs", "ofa"};
    for (const char *prefix : kVideoPrefixes) {
        if (engine.startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

// Resolves a PCI vendor:device pair to a marketing name using the hwdata
// database. Called only while enumerating devices at startup, so a linear
// scan of the file is fine.
QString lookupPciDeviceName(const QString &vendorId, const QString &deviceId)
{
    static const QStringList kPciIdsPaths = {
        QStringLiteral("/usr/share/hwdata/pci.ids"),
        QStringLiteral("/usr/share/misc/pci.ids"),
    };

    for (const QString &path : kPciIdsPaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        // Format: vendors at column 0, their devices indented one tab,
        // subsystems two tabs. Comments start with '#'.
        bool inVendor = false;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine();
            if (line.isEmpty() || line.startsWith('#') || line.startsWith("\t\t")) {
                continue;
            }
            if (line.startsWith('\t')) {
                if (!inVendor) {
                    continue;
                }
                const QByteArray trimmed = line.mid(1);
                if (trimmed.left(4).toLower() == deviceId.toLatin1()) {
                    return QString::fromUtf8(trimmed.mid(4).trimmed());
                }
                continue;
            }
            // A new vendor block: if we were inside the one we wanted, the
            // device is not listed and there is no point reading on.
            if (inVendor) {
                return {};
            }
            inVendor = line.left(4).toLower() == vendorId.toLatin1();
        }
        return {};
    }
    return {};
}

// Used when hwdata is not installed, so a card is still named something
// better than its raw ids.
QString genericVendorName(const QString &vendorId)
{
    if (vendorId == QStringLiteral("1002")) {
        return QStringLiteral("AMD Radeon Graphics");
    }
    if (vendorId == QStringLiteral("10de")) {
        return QStringLiteral("NVIDIA Graphics");
    }
    if (vendorId == QStringLiteral("8086")) {
        return QStringLiteral("Intel Graphics");
    }
    return {};
}

} // namespace

DrmGpuProvider::DrmGpuProvider(QString root)
    : m_root(std::move(root))
{
    discoverDevices();
}

bool DrmGpuProvider::isAvailable() const
{
    return !m_devices.isEmpty();
}

QVector<GpuDeviceInfo> DrmGpuProvider::devices() const
{
    QVector<GpuDeviceInfo> result;
    result.reserve(m_devices.size());
    for (const Device &device : m_devices) {
        result.append(device.info);
    }
    return result;
}

void DrmGpuProvider::discoverDevices()
{
    const QDir drmDir(m_root + QStringLiteral("/sys/class/drm"));
    if (!drmDir.exists()) {
        return;
    }

    // Everything in /sys/class/drm, so the node-to-device map also covers the
    // render nodes; only bare "cardN" entries become devices, since
    // "card0-DP-4" is a connector rather than a GPU.
    const QStringList entries =
        drmDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);

    for (const QString &entry : entries) {
        const QString devicePath = m_root + QStringLiteral("/sys/class/drm/")
            + entry + QStringLiteral("/device");
        const QString deviceId = QFileInfo(devicePath).canonicalFilePath().section(
            QLatin1Char('/'), -1);
        if (deviceId.isEmpty()) {
            continue;
        }
        m_nodeToDevice.insert(entry, deviceId);

        const bool isCardNode = entry.startsWith(QStringLiteral("card"))
            && !entry.contains(QLatin1Char('-'));
        if (!isCardNode) {
            continue;
        }

        const QString driver = QFileInfo(devicePath + QStringLiteral("/driver"))
                                   .canonicalFilePath()
                                   .section(QLatin1Char('/'), -1);
        if (isNonGpuDriver(driver)) {
            continue;
        }

        Device device;
        device.sysfsPath = devicePath;
        device.info.id = deviceId;
        device.info.driver = driver;
        device.engineStatsExpected = engineStatsExpected(driver);

        // sysfs writes PCI ids as "0x1002"; pci.ids keys them bare.
        const QString vendorId =
            QString::fromLatin1(readSysFile(devicePath + QStringLiteral("/vendor")))
                .mid(2);
        const QString productId =
            QString::fromLatin1(readSysFile(devicePath + QStringLiteral("/device")))
                .mid(2);
        device.info.name = lookupPciDeviceName(vendorId, productId);
        if (device.info.name.isEmpty()) {
            device.info.name = genericVendorName(vendorId);
        }
        if (device.info.name.isEmpty()) {
            device.info.name =
                QCoreApplication::translate("DrmGpuProvider", "GPU %1")
                    .arg(deviceId);
        }

        device.info.memTotalBytes =
            readSysFile(devicePath + QStringLiteral("/mem_info_vram_total"))
                .toULongLong();
        const QDir hwmonDir(devicePath + QStringLiteral("/hwmon"));
        const QStringList hwmons =
            hwmonDir.entryList(QStringList{QStringLiteral("hwmon*")},
                               QDir::Dirs | QDir::NoDotAndDotDot);
        if (!hwmons.isEmpty()) {
            device.hwmonPath = hwmonDir.filePath(hwmons.first());
        }

        // The blob driver publishes neither utilization sysfs nodes nor DRM
        // fdinfo keys, so this backend can see the card but never measure it.
        // It is still listed: the registry replaces this entry when the
        // nvidia-smi backend has a real one, and when that tool is missing
        // the row survives to carry the hint.
        if (driver == QStringLiteral("nvidia")) {
            device.info.metricsAvailable = false;
            device.info.unavailableHint =
                optionaltools::missingHint(optionaltools::Tool::NvidiaSmi);
        } else {
            device.info.metricsAvailable = true;
        }

        // The per-client walk runs for any device with engine stats, not just
        // ones missing a sysfs utilization node, because the sysfs nodes are
        // not always truthful: on Raphael, vcn_busy_percent stays at 0 through
        // a VA-API encode that drm-engine-enc correctly reports at ~98%.
        // Deriving in parallel and taking the larger of the two is what makes
        // the video figure meaningful there.
        if (device.info.metricsAvailable && device.engineStatsExpected) {
            m_needDerivedAggregate = true;
        }
        m_devices.append(device);
    }
}

void DrmGpuProvider::setProcessSamplingEnabled(bool enabled)
{
    if (m_processSamplingEnabled == enabled) {
        return;
    }
    m_processSamplingEnabled = enabled;
    if (!enabled) {
        m_processUsage.clear();
    }
}

bool DrmGpuProvider::parseFdInfo(const QByteArray &data, const QString &nodeName,
                                 FdInfoEntry &out) const
{
    // Every DRM fd carries this key; sockets, pipes and regular files do not,
    // which makes it the cheapest possible reject.
    if (!data.contains("drm-driver")) {
        return false;
    }

    const QList<QByteArray> lines = data.split('\n');
    for (const QByteArray &line : lines) {
        if (!line.startsWith("drm-")) {
            continue;
        }
        const int colon = line.indexOf(':');
        if (colon < 0) {
            continue;
        }
        const QByteArray key = line.left(colon);
        const QByteArray value = line.mid(colon + 1).trimmed();

        if (key == "drm-pdev") {
            out.deviceId = QString::fromLatin1(value);
        } else if (key == "drm-client-id") {
            out.clientId = value;
        } else if (key.startsWith("drm-engine-")) {
            // "drm-engine-gfx:\t4148771416 ns"
            const quint64 ns = value.split(' ').first().toULongLong();
            const QByteArray engine = key.mid(int(sizeof("drm-engine-") - 1));
            out.engineNs.insert(engine, ns);
        }
    }

    // Some drivers omit drm-pdev; the device node the fd points at identifies
    // the card just as well.
    if (out.deviceId.isEmpty()) {
        out.deviceId = m_nodeToDevice.value(nodeName);
    }
    out.valid = !out.deviceId.isEmpty() && !out.clientId.isEmpty();
    return out.valid;
}

void DrmGpuProvider::refreshFromFdInfo()
{
    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsedMsec =
        m_havePreviousClients ? nowMsec - m_previousWalkMsec : 0;
    const double elapsedNs = static_cast<double>(elapsedMsec) * 1000000.0;

    QHash<QString, EngineCounters> current;
    QHash<int, GpuProcessUsage> usage;
    QHash<QString, double> deviceGraphics;
    QHash<QString, double> deviceVideo;

    // Raw opendir/readdir rather than QDir: this walks every fd of every
    // readable process, and QDir::entryList() stats each entry, which more
    // than doubles the cost of a walk that only needs the names.
    DIR *procDir = opendir("/proc");
    if (!procDir) {
        return;
    }

    while (const dirent *procEntry = readdir(procDir)) {
        if (procEntry->d_name[0] < '0' || procEntry->d_name[0] > '9') {
            continue;
        }
        bool pidOk = false;
        const int pid = QByteArray(procEntry->d_name).toInt(&pidOk);
        if (!pidOk) {
            continue;
        }

        const QString fdDirPath = QStringLiteral("/proc/%1/fd").arg(pid);
        DIR *fdDir = opendir(QFile::encodeName(fdDirPath).constData());
        if (!fdDir) {
            continue; // another user's process, or it just exited
        }

        // The directory opened, so this process's GPU use is knowable: even
        // if it holds no DRM fd at all, that is a measured zero rather than
        // the unknown a negative value stands for.
        GpuProcessUsage &processUsage = usage[pid];
        if (processUsage.graphicsPercent < 0.0) {
            processUsage.graphicsPercent = 0.0;
            processUsage.videoPercent = 0.0;
        }

        while (const dirent *fdEntry = readdir(fdDir)) {
            if (fdEntry->d_name[0] == '.') {
                continue;
            }
            const QString fdPath =
                fdDirPath + QLatin1Char('/') + QString::fromLatin1(fdEntry->d_name);

            char target[256];
            const ssize_t length =
                readlink(QFile::encodeName(fdPath).constData(), target, sizeof(target) - 1);
            if (length <= 0) {
                continue;
            }
            target[length] = '\0';
            if (qstrncmp(target, "/dev/dri/", 9) != 0) {
                continue;
            }
            const QString nodeName = QString::fromLatin1(target + 9);

            const QString fdInfoPath = QStringLiteral("/proc/%1/fdinfo/%2")
                                           .arg(pid)
                                           .arg(QString::fromLatin1(fdEntry->d_name));
            FdInfoEntry entry;
            if (!parseFdInfo(readSysFile(fdInfoPath), nodeName, entry)) {
                continue;
            }

            // Several fds can reference one client, each repeating its
            // counters; count each client once.
            const QString clientKey = entry.deviceId + QLatin1Char('/')
                + QString::fromLatin1(entry.clientId);
            if (current.contains(clientKey)) {
                continue;
            }

            const EngineCounters previous = m_previousClients.value(clientKey);
            double graphicsNs = 0.0;
            double videoNs = 0.0;
            for (auto it = entry.engineNs.cbegin(); it != entry.engineNs.cend(); ++it) {
                const quint64 before = previous.value(it.key(), it.value());
                const quint64 delta = it.value() > before ? it.value() - before : 0;
                // Max, not sum: engines run concurrently, so a client with
                // gfx and dma both busy for a whole second has used one
                // second of GPU time, not two.
                double &target = isVideoEngine(it.key()) ? videoNs : graphicsNs;
                target = qMax(target, static_cast<double>(delta));
            }
            current.insert(clientKey, entry.engineNs);

            if (elapsedNs <= 0.0) {
                continue; // first walk: counters recorded, no delta yet
            }
            const double graphicsPercent = 100.0 * graphicsNs / elapsedNs;
            const double videoPercent = 100.0 * videoNs / elapsedNs;

            processUsage.graphicsPercent += graphicsPercent;
            processUsage.videoPercent += videoPercent;
            deviceGraphics[entry.deviceId] += graphicsPercent;
            deviceVideo[entry.deviceId] += videoPercent;
        }
        closedir(fdDir);
    }
    closedir(procDir);

    // A process can drive more than one client, and more than one GPU; the
    // columns show a single figure, so the shares add up and saturate.
    for (auto it = usage.begin(); it != usage.end(); ++it) {
        it->graphicsPercent = qMin(100.0, it->graphicsPercent);
        it->videoPercent = qMin(100.0, it->videoPercent);
    }

    // Only meaningful for processes we can actually read, which excludes
    // other users' — the same limitation the disk columns have.
    for (const Device &device : m_devices) {
        if (!device.engineStatsExpected) {
            continue;
        }
        m_derivedBusyPercent.insert(device.info.id,
                                    qMin(100.0, deviceGraphics.value(device.info.id)));
        m_derivedVideoPercent.insert(device.info.id,
                                     qMin(100.0, deviceVideo.value(device.info.id)));
    }

    m_processUsage = std::move(usage);
    m_previousClients = std::move(current);
    m_previousWalkMsec = nowMsec;
    m_havePreviousClients = true;
}

QVector<GpuSnapshot> DrmGpuProvider::sample()
{
    if (m_processSamplingEnabled || m_needDerivedAggregate) {
        refreshFromFdInfo();
    } else if (m_havePreviousClients) {
        // Stale counters would produce a huge fake delta on the next walk,
        // and stale derived percentages would be served as live readings.
        m_previousClients.clear();
        m_derivedBusyPercent.clear();
        m_derivedVideoPercent.clear();
        m_havePreviousClients = false;
    }

    QVector<GpuSnapshot> snapshots;
    snapshots.reserve(m_devices.size());

    for (const Device &device : m_devices) {
        if (!device.info.metricsAvailable) {
            continue;
        }

        GpuSnapshot snapshot;
        snapshot.id = device.info.id;
        snapshot.memTotalBytes = device.info.memTotalBytes;

        // The derived figure wins wherever it exists, and sysfs is the
        // fallback — the opposite of what the file names suggest.
        //
        // gpu_busy_percent is an instantaneous register read, not an average:
        // polled in a tight loop under one steady VA-API encode it returns
        // 0, 43 and 54 within the same second, so sampling it once a tick
        // draws a graph that jitters at random. The engine counters behind
        // the derived value are cumulative nanoseconds, so a delta over
        // elapsed wall time integrates the whole interval and is both stable
        // and correctly split between graphics and video — vcn_busy_percent
        // meanwhile reports a flat 0 through an encode that drm-engine-enc
        // puts at 98%.
        //
        // What sysfs still buys is coverage: the derived figure only counts
        // clients owned by processes this user can read, so GPU work done by
        // another user's process is invisible to it and shows up only here.
        const auto combine = [](double sysfsValue, bool sysfsOk, double derived,
                                bool haveDerived) {
            if (haveDerived) {
                return derived;
            }
            return sysfsOk ? sysfsValue : -1.0;
        };

        bool ok = false;
        const double busy =
            readSysFile(device.sysfsPath + QStringLiteral("/gpu_busy_percent"))
                .toDouble(&ok);
        snapshot.busyPercent =
            combine(busy, ok, m_derivedBusyPercent.value(device.info.id),
                    m_derivedBusyPercent.contains(device.info.id));

        // amdgpu exposes the video block separately; other drivers only
        // reveal it through per-client engine counters.
        const double videoBusy =
            readSysFile(device.sysfsPath + QStringLiteral("/vcn_busy_percent"))
                .toDouble(&ok);
        snapshot.videoPercent =
            combine(videoBusy, ok, m_derivedVideoPercent.value(device.info.id),
                    m_derivedVideoPercent.contains(device.info.id));

        snapshot.memUsedBytes =
            readSysFile(device.sysfsPath + QStringLiteral("/mem_info_vram_used"))
                .toULongLong();

        if (!device.hwmonPath.isEmpty()) {
            // hwmon units: millidegrees, microwatts, hertz.
            const double milliDegrees =
                readSysFile(device.hwmonPath + QStringLiteral("/temp1_input"))
                    .toDouble(&ok);
            if (ok) {
                snapshot.temperatureC = milliDegrees / 1000.0;
            }
            double microWatts =
                readSysFile(device.hwmonPath + QStringLiteral("/power1_input"))
                    .toDouble(&ok);
            if (!ok) {
                microWatts =
                    readSysFile(device.hwmonPath + QStringLiteral("/power1_average"))
                        .toDouble(&ok);
            }
            if (ok) {
                snapshot.powerWatts = microWatts / 1000000.0;
            }
            const double hertz =
                readSysFile(device.hwmonPath + QStringLiteral("/freq1_input"))
                    .toDouble(&ok);
            if (ok) {
                snapshot.clockMhz = hertz / 1000000.0;
            }
        }

        snapshots.append(snapshot);
    }
    return snapshots;
}

QHash<int, GpuProcessUsage> DrmGpuProvider::sampleProcesses()
{
    // Filled by the walk sample() just ran; see IGpuProvider's contract.
    return m_processSamplingEnabled ? m_processUsage : QHash<int, GpuProcessUsage>{};
}
