// Fixture-based tests for the /proc and /sys parsers.
//
// Every provider takes a filesystem root that is prepended to the paths it
// reads. The application leaves it empty and gets the real system; these
// tests point it at a temporary directory seeded from tests/fixtures, so the
// parsing and the delta arithmetic run against known input.
//
// The delta arithmetic is the reason this file exists. Rates are a counter
// difference divided by elapsed time, and that is exactly the kind of code
// that fails silently: a wrong divisor, an unhandled counter reset or an
// off-by-one in a column index still produces a number that looks plausible
// on screen. Each case below pins one of those down to an exact expected
// value.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "providers/cpuprovider.h"
#include "providers/diskprovider.h"
#include "providers/drmgpuprovider.h"
#include "providers/gpusensors.h"
#include "providers/hwmonprovider.h"
#include "providers/memoryprovider.h"
#include "providers/networkprovider.h"

namespace {

constexpr double kEpsilon = 1e-6;

QString fixturePath(const QString &name)
{
    return QStringLiteral(QTMONITOR_FIXTURE_DIR "/") + name;
}

// Copies a fixture to <root>/<relativePath>, creating the directories. Used
// both to seed a root and to swap a file between two samples, which is how a
// counter delta is staged.
bool place(const QString &root, const QString &fixture, const QString &relativePath)
{
    const QString target = root + QLatin1Char('/') + relativePath;
    if (!QDir().mkpath(QFileInfo(target).path())) {
        return false;
    }
    QFile::remove(target);
    return QFile::copy(fixturePath(fixture), target);
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).path())) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(contents) == contents.size();
}

// Minimal /sys/block entry: enough attributes for enumerateDevices() to
// accept the device and read its size.
bool makeBlockDevice(const QString &root, const QString &name, quint64 sectors,
                     bool rotational, bool removable)
{
    const QString base = root + QStringLiteral("/sys/block/") + name;
    return writeFile(base + QStringLiteral("/size"),
                     QByteArray::number(qulonglong(sectors)) + "\n")
        && writeFile(base + QStringLiteral("/queue/rotational"),
                     rotational ? "1\n" : "0\n")
        && writeFile(base + QStringLiteral("/removable"),
                     removable ? "1\n" : "0\n");
}

// Minimal /sys/class/net entry. `deviceTarget` non-empty makes the interface
// look physical by creating the "device" symlink the provider probes for.
bool makeNetDevice(const QString &root, const QString &name, int type,
                   const QString &deviceTarget)
{
    const QString base = root + QStringLiteral("/sys/class/net/") + name;
    if (!writeFile(base + QStringLiteral("/type"), QByteArray::number(type) + "\n")
        || !writeFile(base + QStringLiteral("/operstate"), "up\n")
        || !writeFile(base + QStringLiteral("/mtu"), "1500\n")) {
        return false;
    }
    if (deviceTarget.isEmpty()) {
        return true;
    }
    const QString devicesDir = root + QStringLiteral("/sys/devices/") + deviceTarget;
    return QDir().mkpath(devicesDir)
        && QFile::link(devicesDir, base + QStringLiteral("/device"));
}

// Minimal /sys/class/drm entry. A GPU is a bare "cardN" whose device symlink
// resolves into the device tree and whose driver symlink names the module.
bool makeDrmCard(const QString &root, const QString &node, const QString &pciAddress,
                 const QString &driver, const QString &vendorId)
{
    const QString devicesDir =
        root + QStringLiteral("/sys/devices/pci0000:00/") + pciAddress;
    const QString driversDir = root + QStringLiteral("/sys/bus/pci/drivers/") + driver;
    if (!QDir().mkpath(devicesDir) || !QDir().mkpath(driversDir)) {
        return false;
    }
    if (!vendorId.isEmpty()
        && !writeFile(devicesDir + QStringLiteral("/vendor"),
                      ("0x" + vendorId + "\n").toLatin1())) {
        return false;
    }
    if (!writeFile(devicesDir + QStringLiteral("/device"), "0x1234\n")) {
        return false;
    }
    if (!QFile::exists(devicesDir + QStringLiteral("/driver"))
        && !QFile::link(driversDir, devicesDir + QStringLiteral("/driver"))) {
        return false;
    }
    const QString nodeDir = root + QStringLiteral("/sys/class/drm/") + node;
    return QDir().mkpath(nodeDir)
        && QFile::link(devicesDir, nodeDir + QStringLiteral("/device"));
}

// Minimal hwmon chip. `attributes` maps a sysfs attribute name to its raw
// contents, so a test writes exactly the registers the real chip publishes —
// including the ones that publish nonsense.
bool makeHwmonChip(const QString &root, const QString &node, const QString &name,
                   const QHash<QString, QByteArray> &attributes)
{
    const QString base = root + QStringLiteral("/sys/class/hwmon/") + node;
    if (!writeFile(base + QStringLiteral("/name"), name.toUtf8() + "\n")) {
        return false;
    }
    for (auto it = attributes.cbegin(); it != attributes.cend(); ++it) {
        if (!writeFile(base + QLatin1Char('/') + it.key(), it.value() + "\n")) {
            return false;
        }
    }
    return true;
}

// Finds a chip by its sysfs node name; the provider sorts, so index is not
// a safe way to address one.
const SensorChipInfo *chipById(const QVector<SensorChipInfo> &chips, const QString &id)
{
    for (const SensorChipInfo &chip : chips) {
        if (chip.id == id) {
            return &chip;
        }
    }
    return nullptr;
}

double readingFor(const QVector<SensorChipSnapshot> &snapshots, const QString &chipId,
                  const QString &channelId)
{
    for (const SensorChipSnapshot &snapshot : snapshots) {
        if (snapshot.chipId != chipId) {
            continue;
        }
        for (const SensorReading &reading : snapshot.readings) {
            if (reading.channelId == channelId) {
                return reading.value;
            }
        }
    }
    return -99.0; // distinct from the -1 that means "present but unreadable"
}

const SensorChannelInfo *channelById(const SensorChipInfo &chip, const QString &id)
{
    for (const SensorChannelInfo &channel : chip.channels) {
        if (channel.id == id) {
            return &channel;
        }
    }
    return nullptr;
}

} // namespace

class TestProviders : public QObject
{
    Q_OBJECT

private slots:
    void cpuFirstSampleReportsNoUsage();
    void cpuDerivesPerCoreUtilisation();
    void cpuCoreCountExcludesAggregateLine();
    void cpuMeanFrequency();
    void cpuMissingProcIsNotAvailable();

    void memoryParsesMeminfo();
    void memoryUsedIsTotalMinusAvailable();
    void memorySwapUsedIsTotalMinusFree();

    void networkFirstSampleHasNoRate();
    void networkDerivesRatesFromDelta();
    void networkCounterResetProducesNoRate();
    void networkParsesNameWithNoSpaceAfterColon();
    void networkExcludesLoopback();

    void diskFirstSampleHasNoRate();
    void diskDerivesRatesFromDelta();
    void diskActivePercentIsClamped();
    void diskCounterResetProducesNoRate();
    void diskEnumeratesWholeDevicesOnly();

    void gpuDiscoversCardNodesNotConnectors();
    void gpuSkipsNonGpuDrivers();
    void gpuMarksNvidiaUnmeasurable();

    void sensorsScalesEachChannelKind();
    void sensorsDoesNotScaleFanSpeed();
    void sensorsSynthesizesLabelWhenChipPublishesNone();
    void sensorsRejectsImplausibleLimits();
    void sensorsUnreadableChannelIsNotZero();
    void sensorsSkipsChipsWithNoReadableChannel();
    void sensorsNumbersOnlyAmbiguousDisplayNames();
    void sensorsIgnoresIntrusionAttribute();
    void sensorsOrdersChipsNumerically();
    void sensorsFallsBackToPowerAverage();
    void sensorsRecordsParentDevice();
    void gpuSensorsListOnlyCardsHwmonCannotSee();
    void gpuSensorsKeepMeasuredZeroDistinctFromUnknown();
};

// --------------------------------------------------------------------- CPU

void TestProviders::cpuFirstSampleReportsNoUsage()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(place(root.path(), QStringLiteral("proc-stat-t0"),
                  QStringLiteral("proc/stat")));

    ProcfsCpuProvider provider(root.path());
    const CpuSnapshot first = provider.sample();

    // No previous reading exists, so there is no delta to divide. Reporting
    // anything but zero here would mean deriving utilisation from totals
    // accumulated since boot.
    QCOMPARE(first.totalPercent, 0.0);
    for (const double percent : first.perCorePercents) {
        QCOMPARE(percent, 0.0);
    }
}

void TestProviders::cpuDerivesPerCoreUtilisation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(place(root.path(), QStringLiteral("proc-stat-t0"),
                  QStringLiteral("proc/stat")));

    ProcfsCpuProvider provider(root.path());
    provider.sample(); // baseline

    QVERIFY(place(root.path(), QStringLiteral("proc-stat-t1"),
                  QStringLiteral("proc/stat")));
    const CpuSnapshot second = provider.sample();

    // Between t0 and t1 every core advances 100 total jiffies, of which
    // 100 / 50 / 0 / 75 are idle.
    QCOMPARE(second.perCorePercents.size(), 4);
    QVERIFY(qAbs(second.perCorePercents.at(0) - 0.0) < kEpsilon);
    QVERIFY(qAbs(second.perCorePercents.at(1) - 50.0) < kEpsilon);
    QVERIFY(qAbs(second.perCorePercents.at(2) - 100.0) < kEpsilon);
    QVERIFY(qAbs(second.perCorePercents.at(3) - 25.0) < kEpsilon);

    // The aggregate line is read directly rather than averaged, but it must
    // agree with the mean of the cores: 400 total, 225 idle.
    QVERIFY(qAbs(second.totalPercent - 43.75) < kEpsilon);

    QCOMPARE(second.contextSwitches, quint64(9876543));
    QCOMPARE(second.interrupts, quint64(1234567));
}

void TestProviders::cpuCoreCountExcludesAggregateLine()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(place(root.path(), QStringLiteral("proc-stat-t0"),
                  QStringLiteral("proc/stat")));

    ProcfsCpuProvider provider(root.path());
    // /proc/stat carries five "cpu" lines: one aggregate and four cores. An
    // off-by-one here would put a phantom core in the per-core grid.
    QCOMPARE(provider.sample().coreCount, 4);
}

void TestProviders::cpuMeanFrequency()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(place(root.path(), QStringLiteral("proc-stat-t0"),
                  QStringLiteral("proc/stat")));
    QVERIFY(place(root.path(), QStringLiteral("proc-cpuinfo"),
                  QStringLiteral("proc/cpuinfo")));

    ProcfsCpuProvider provider(root.path());
    // 2400 + 3200 + 3600 + 2800 = 12000 MHz over four cores = 3.0 GHz.
    QVERIFY(qAbs(provider.sample().currentFreqGhz - 3.0) < kEpsilon);
}

void TestProviders::cpuMissingProcIsNotAvailable()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    ProcfsCpuProvider provider(root.path());
    QVERIFY(!provider.isAvailable());
}

// ------------------------------------------------------------------ Memory

void TestProviders::memoryParsesMeminfo()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(place(root.path(), QStringLiteral("proc-meminfo"),
                  QStringLiteral("proc/meminfo")));

    ProcfsMemoryProvider provider(root.path());
    QVERIFY(provider.isAvailable());
    const MemorySnapshot snapshot = provider.sample();

    // meminfo is in KiB; the snapshot is in bytes.
    QCOMPARE(snapshot.memTotalBytes, quint64(32768000) * 1024);
    QCOMPARE(snapshot.memAvailableBytes, quint64(16384000) * 1024);
    QCOMPARE(snapshot.swapTotalBytes, quint64(8388608) * 1024);
    QCOMPARE(snapshot.swapFreeBytes, quint64(4194304) * 1024);
}

void TestProviders::memoryUsedIsTotalMinusAvailable()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(place(root.path(), QStringLiteral("proc-meminfo"),
                  QStringLiteral("proc/meminfo")));

    ProcfsMemoryProvider provider(root.path());
    const MemorySnapshot snapshot = provider.sample();

    // MemAvailable, not MemFree: cache and reclaimable slab are available to
    // a new allocation, so counting them as used overstates pressure. That
    // choice is the difference between 16 GiB and 24 GiB used here.
    QCOMPARE(snapshot.memUsedBytes, quint64(32768000 - 16384000) * 1024);
}

void TestProviders::memorySwapUsedIsTotalMinusFree()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(place(root.path(), QStringLiteral("proc-meminfo"),
                  QStringLiteral("proc/meminfo")));

    ProcfsMemoryProvider provider(root.path());
    QCOMPARE(provider.sample().swapUsedBytes, quint64(8388608 - 4194304) * 1024);
}

// ----------------------------------------------------------------- Network

void TestProviders::networkFirstSampleHasNoRate()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeNetDevice(root.path(), QStringLiteral("eth0"), 1,
                          QStringLiteral("pci0000:00/0000:4d:00.0")));
    QVERIFY(place(root.path(), QStringLiteral("proc-net-dev-t0"),
                  QStringLiteral("proc/net/dev")));

    ProcfsNetworkProvider provider(root.path());
    const QVector<NetworkSnapshot> first = provider.sampleWithElapsed(0.0);
    QCOMPARE(first.size(), 1);
    QCOMPARE(first.first().receiveBytesPerSec, 0.0);
    QCOMPARE(first.first().sendBytesPerSec, 0.0);
    // Totals are still reported on the first sample; only the rates wait.
    QCOMPARE(first.first().receiveTotalBytes, quint64(1000000));
}

void TestProviders::networkDerivesRatesFromDelta()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeNetDevice(root.path(), QStringLiteral("eth0"), 1,
                          QStringLiteral("pci0000:00/0000:4d:00.0")));
    QVERIFY(place(root.path(), QStringLiteral("proc-net-dev-t0"),
                  QStringLiteral("proc/net/dev")));

    ProcfsNetworkProvider provider(root.path());
    provider.sampleWithElapsed(0.0);

    QVERIFY(place(root.path(), QStringLiteral("proc-net-dev-t1"),
                  QStringLiteral("proc/net/dev")));
    const QVector<NetworkSnapshot> second = provider.sampleWithElapsed(2000.0);

    QCOMPARE(second.size(), 1);
    // +1048576 bytes received over 2 s = 512 KiB/s; +524288 sent = 256 KiB/s.
    // The interval is supplied rather than measured, so this is exact.
    QVERIFY(qAbs(second.first().receiveBytesPerSec - 524288.0) < kEpsilon);
    QVERIFY(qAbs(second.first().sendBytesPerSec - 262144.0) < kEpsilon);
    QCOMPARE(second.first().receiveDrops, quint64(14));
}

void TestProviders::networkCounterResetProducesNoRate()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeNetDevice(root.path(), QStringLiteral("wlan0"), 1,
                          QStringLiteral("pci0000:00/0000:03:00.0")));
    QVERIFY(place(root.path(), QStringLiteral("proc-net-dev-t0"),
                  QStringLiteral("proc/net/dev")));

    ProcfsNetworkProvider provider(root.path());
    provider.sampleWithElapsed(0.0);

    QVERIFY(place(root.path(), QStringLiteral("proc-net-dev-t1"),
                  QStringLiteral("proc/net/dev")));
    const QVector<NetworkSnapshot> second = provider.sampleWithElapsed(2000.0);

    // wlan0's counters go backwards between the two fixtures, which is what
    // an interface that went away and came back looks like. Subtracting
    // unsigned counters in the wrong order would wrap to an enormous
    // positive rate and spike the graph, so the interval is skipped instead.
    QCOMPARE(second.size(), 1);
    QCOMPARE(second.first().receiveBytesPerSec, 0.0);
    QCOMPARE(second.first().sendBytesPerSec, 0.0);
}

void TestProviders::networkParsesNameWithNoSpaceAfterColon()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeNetDevice(root.path(), QStringLiteral("verylongifname0"), 1,
                          QStringLiteral("pci0000:00/0000:05:00.0")));
    QVERIFY(place(root.path(), QStringLiteral("proc-net-dev-t0"),
                  QStringLiteral("proc/net/dev")));

    ProcfsNetworkProvider provider(root.path());
    const QVector<NetworkSnapshot> snapshots = provider.sampleWithElapsed(0.0);

    // The name field is right-aligned in a fixed width, so a long enough name
    // leaves no space between the colon and the first value. Splitting the
    // line on whitespace would fold the name and the byte count into one
    // token and silently lose the interface.
    QCOMPARE(snapshots.size(), 1);
    QCOMPARE(snapshots.first().id, QStringLiteral("verylongifname0"));
    QCOMPARE(snapshots.first().receiveTotalBytes, quint64(12345678));
    QCOMPARE(snapshots.first().sendTotalBytes, quint64(87654321));
}

void TestProviders::networkExcludesLoopback()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeNetDevice(root.path(), QStringLiteral("lo"), 772, QString()));
    QVERIFY(makeNetDevice(root.path(), QStringLiteral("eth0"), 1,
                          QStringLiteral("pci0000:00/0000:4d:00.0")));
    QVERIFY(place(root.path(), QStringLiteral("proc-net-dev-t0"),
                  QStringLiteral("proc/net/dev")));

    ProcfsNetworkProvider provider(root.path());
    const QVector<NetworkDeviceInfo> devices = provider.devices();

    // Loopback is always up and always fast; a permanent sidebar row for it
    // would say nothing about the machine's connectivity.
    QCOMPARE(devices.size(), 1);
    QCOMPARE(devices.first().id, QStringLiteral("eth0"));
}

// -------------------------------------------------------------------- Disk

void TestProviders::diskFirstSampleHasNoRate()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeBlockDevice(root.path(), QStringLiteral("nvme0n1"), 2000409264,
                            false, false));
    QVERIFY(place(root.path(), QStringLiteral("proc-diskstats-t0"),
                  QStringLiteral("proc/diskstats")));

    ProcfsDiskProvider provider(root.path());
    const QVector<DiskSnapshot> first = provider.sampleWithElapsed(0.0);
    QCOMPARE(first.size(), 1);
    QCOMPARE(first.first().readBytesPerSec, 0.0);
    QCOMPARE(first.first().writeBytesPerSec, 0.0);
    QCOMPARE(first.first().activePercent, 0.0);
}

void TestProviders::diskDerivesRatesFromDelta()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeBlockDevice(root.path(), QStringLiteral("nvme0n1"), 2000409264,
                            false, false));
    QVERIFY(place(root.path(), QStringLiteral("proc-diskstats-t0"),
                  QStringLiteral("proc/diskstats")));

    ProcfsDiskProvider provider(root.path());
    provider.sampleWithElapsed(0.0);

    QVERIFY(place(root.path(), QStringLiteral("proc-diskstats-t1"),
                  QStringLiteral("proc/diskstats")));
    const QVector<DiskSnapshot> second = provider.sampleWithElapsed(2000.0);

    QCOMPARE(second.size(), 1);
    const DiskSnapshot &disk = second.first();

    // diskstats counts 512-byte sectors regardless of the device's real
    // block size. +4096 sectors read = 2 MiB over 2 s = 1 MiB/s.
    QVERIFY(qAbs(disk.readBytesPerSec - 1048576.0) < kEpsilon);
    // +2048 sectors written = 1 MiB over 2 s = 512 KiB/s.
    QVERIFY(qAbs(disk.writeBytesPerSec - 524288.0) < kEpsilon);
    // io_ticks advanced 500 ms across a 2000 ms interval.
    QVERIFY(qAbs(disk.activePercent - 25.0) < kEpsilon);
    // Weighted io_ticks advanced 3000 ms across 2000 ms: a mean of 1.5
    // requests in flight. This is deliberately allowed to exceed 1 — that is
    // what a queue building up looks like.
    QVERIFY(qAbs(disk.avgQueueLength - 1.5) < kEpsilon);
    QCOMPARE(disk.inFlight, 2);
}

void TestProviders::diskActivePercentIsClamped()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeBlockDevice(root.path(), QStringLiteral("zram0"), 16777216,
                            false, false));
    QVERIFY(place(root.path(), QStringLiteral("proc-diskstats-t0"),
                  QStringLiteral("proc/diskstats")));

    ProcfsDiskProvider provider(root.path());
    provider.sampleWithElapsed(0.0);

    QVERIFY(place(root.path(), QStringLiteral("proc-diskstats-t1"),
                  QStringLiteral("proc/diskstats")));
    const QVector<DiskSnapshot> second = provider.sampleWithElapsed(2000.0);

    // zram0's io_ticks climbs 2500 ms across a 2000 ms interval, which cannot
    // happen physically but does happen from rounding at both ends. 125%
    // active would be drawn off the top of the graph.
    QCOMPARE(second.size(), 1);
    QVERIFY(qAbs(second.first().activePercent - 100.0) < kEpsilon);
}

void TestProviders::diskCounterResetProducesNoRate()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeBlockDevice(root.path(), QStringLiteral("sda"), 3907029168,
                            true, false));
    QVERIFY(place(root.path(), QStringLiteral("proc-diskstats-t0"),
                  QStringLiteral("proc/diskstats")));

    ProcfsDiskProvider provider(root.path());
    provider.sampleWithElapsed(0.0);

    QVERIFY(place(root.path(), QStringLiteral("proc-diskstats-t1"),
                  QStringLiteral("proc/diskstats")));
    const QVector<DiskSnapshot> second = provider.sampleWithElapsed(2000.0);

    // sda's counters go backwards, as they do for a removable disk that was
    // unplugged and reattached. The interval is skipped rather than wrapped.
    QCOMPARE(second.size(), 1);
    QCOMPARE(second.first().readBytesPerSec, 0.0);
    QCOMPARE(second.first().writeBytesPerSec, 0.0);
    QCOMPARE(second.first().activePercent, 0.0);
}

void TestProviders::diskEnumeratesWholeDevicesOnly()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeBlockDevice(root.path(), QStringLiteral("nvme0n1"), 2000409264,
                            false, false));
    QVERIFY(makeBlockDevice(root.path(), QStringLiteral("sda"), 3907029168,
                            true, false));
    QVERIFY(place(root.path(), QStringLiteral("proc-diskstats-t0"),
                  QStringLiteral("proc/diskstats")));

    ProcfsDiskProvider provider(root.path());
    const QVector<DiskDeviceInfo> devices = provider.devices();

    // /sys/block lists whole devices only; nvme0n1p1 appears in diskstats but
    // must not get a page of its own, because its I/O is already counted in
    // its disk's.
    QCOMPARE(devices.size(), 2);
    QStringList ids;
    for (const DiskDeviceInfo &device : devices) {
        ids.append(device.id);
    }
    ids.sort();
    QCOMPARE(ids, QStringList({QStringLiteral("nvme0n1"), QStringLiteral("sda")}));

    // sizeBytes is sectors * 512, the same unit diskstats uses.
    for (const DiskDeviceInfo &device : devices) {
        if (device.id == QLatin1String("sda")) {
            QCOMPARE(device.sizeBytes, quint64(3907029168) * 512);
            QVERIFY(device.rotational);
        }
    }
}

// --------------------------------------------------------------------- GPU

void TestProviders::gpuDiscoversCardNodesNotConnectors()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeDrmCard(root.path(), QStringLiteral("card0"),
                        QStringLiteral("0000:03:00.0"), QStringLiteral("amdgpu"),
                        QStringLiteral("1002")));
    // Connectors and render nodes live in the same directory. Only the bare
    // cardN entry is a GPU; "card0-DP-1" is one of its outputs.
    QVERIFY(QDir().mkpath(root.path() + QStringLiteral("/sys/class/drm/card0-DP-1")));
    QVERIFY(QDir().mkpath(root.path() + QStringLiteral("/sys/class/drm/card0-HDMI-A-1")));

    DrmGpuProvider provider(root.path());
    const QVector<GpuDeviceInfo> devices = provider.devices();
    QCOMPARE(devices.size(), 1);
    QCOMPARE(devices.first().driver, QStringLiteral("amdgpu"));
    QVERIFY(devices.first().metricsAvailable);
}

void TestProviders::gpuSkipsNonGpuDrivers()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeDrmCard(root.path(), QStringLiteral("card0"),
                        QStringLiteral("0000:03:00.0"), QStringLiteral("amdgpu"),
                        QStringLiteral("1002")));
    // simpledrm is the kernel's framebuffer shim, present before a real
    // driver loads. Listing it would put a phantom card in the sidebar.
    QVERIFY(makeDrmCard(root.path(), QStringLiteral("card1"),
                        QStringLiteral("0000:00:02.0"), QStringLiteral("simpledrm"),
                        QStringLiteral("8086")));

    DrmGpuProvider provider(root.path());
    QCOMPARE(provider.devices().size(), 1);
}

void TestProviders::gpuMarksNvidiaUnmeasurable()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeDrmCard(root.path(), QStringLiteral("card0"),
                        QStringLiteral("0000:01:00.0"), QStringLiteral("nvidia"),
                        QStringLiteral("10de")));

    DrmGpuProvider provider(root.path());
    const QVector<GpuDeviceInfo> devices = provider.devices();

    // The proprietary driver publishes neither utilisation sysfs nodes nor
    // DRM fdinfo keys. The card is still listed, but marked unmeasurable and
    // carrying the reason, so the page explains itself instead of drawing a
    // flat zero.
    QCOMPARE(devices.size(), 1);
    QVERIFY(!devices.first().metricsAvailable);
    QVERIFY(!devices.first().unavailableHint.isEmpty());
}

// ----------------------------------------------------------------- Sensors

void TestProviders::sensorsScalesEachChannelKind()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    // One chip publishing one channel of every family, with values chosen so
    // a wrong divisor cannot coincidentally produce the right answer.
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon0"),
                          QStringLiteral("nct6799"),
                          {{QStringLiteral("temp1_input"), "42500"},    // mC
                           {QStringLiteral("in0_input"), "1225"},       // mV
                           {QStringLiteral("power1_input"), "65500000"}, // uW
                           {QStringLiteral("curr1_input"), "8250"}}));  // mA

    HwmonSensorProvider provider(root.path());
    const QVector<SensorChipSnapshot> snapshots = provider.sample();

    QCOMPARE(readingFor(snapshots, QStringLiteral("hwmon0"), QStringLiteral("temp1")), 42.5);
    QCOMPARE(readingFor(snapshots, QStringLiteral("hwmon0"), QStringLiteral("in0")), 1.225);
    QCOMPARE(readingFor(snapshots, QStringLiteral("hwmon0"), QStringLiteral("power1")), 65.5);
    QCOMPARE(readingFor(snapshots, QStringLiteral("hwmon0"), QStringLiteral("curr1")), 8.25);
}

void TestProviders::sensorsDoesNotScaleFanSpeed()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon0"),
                          QStringLiteral("nct6799"),
                          {{QStringLiteral("fan1_input"), "1200"}}));

    HwmonSensorProvider provider(root.path());

    // The whole point of this case. Every other hwmon family is scaled, so
    // "divide by 1000" looks like the rule; fan speed is already RPM. Getting
    // it wrong turns 1200 RPM into 1.2, which is a number a reader would
    // never question on a chart.
    QCOMPARE(readingFor(provider.sample(), QStringLiteral("hwmon0"),
                        QStringLiteral("fan1")),
             1200.0);
}

void TestProviders::sensorsSynthesizesLabelWhenChipPublishesNone()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon0"),
                          QStringLiteral("spd5118"),
                          {{QStringLiteral("temp1_input"), "46250"}}));
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon1"),
                          QStringLiteral("k10temp"),
                          {{QStringLiteral("temp1_input"), "60125"},
                           {QStringLiteral("temp1_label"), "Tctl"}}));

    HwmonSensorProvider provider(root.path());
    const QVector<SensorChipInfo> chips = provider.chips();

    // Labels are absent far more often than present — on this project's
    // reference machine only nvme, k10temp and amdgpu publish them — so the
    // synthesized name is the common path, and it must never be the bare
    // sysfs id.
    const SensorChipInfo *unlabelled = chipById(chips, QStringLiteral("hwmon0"));
    QVERIFY(unlabelled);
    QCOMPARE(unlabelled->channels.size(), 1);
    QVERIFY(!unlabelled->channels.first().label.isEmpty());
    QVERIFY(unlabelled->channels.first().label != QLatin1String("temp1"));

    const SensorChipInfo *labelled = chipById(chips, QStringLiteral("hwmon1"));
    QVERIFY(labelled);
    QCOMPARE(labelled->channels.first().label, QStringLiteral("Tctl"));
}

void TestProviders::sensorsRejectsImplausibleLimits()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    // Verbatim from this project's reference machine: the NVMe controller
    // publishes a real critical limit on temp1 and the spec's "threshold not
    // implemented" sentinel on temp2 — 65261850 mC, which is exactly
    // 65535.0 K, i.e. 0xFFFF.
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon0"),
                          QStringLiteral("nvme"),
                          {{QStringLiteral("temp1_input"), "44850"},
                           {QStringLiteral("temp1_crit"), "84850"},
                           {QStringLiteral("temp2_input"), "44850"},
                           {QStringLiteral("temp2_max"), "65261850"}}));

    HwmonSensorProvider provider(root.path());
    const SensorChipInfo *chip = chipById(provider.chips(), QStringLiteral("hwmon0"));
    QVERIFY(chip);

    const SensorChannelInfo *real = channelById(*chip, QStringLiteral("temp1"));
    QVERIFY(real);
    QCOMPARE(real->criticalLimit, 84.85);

    // The sentinel must be dropped as firmly as a missing attribute, or the
    // UI renders "65261.9 C" next to a 44 C reading.
    const SensorChannelInfo *sentinel = channelById(*chip, QStringLiteral("temp2"));
    QVERIFY(sentinel);
    QCOMPARE(sentinel->highLimit, -1.0);
}

void TestProviders::sensorsUnreadableChannelIsNotZero()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    // A powered-down Wi-Fi adapter answers ENODATA rather than a number: the
    // attribute is still there, still enumerated, and open() still succeeds —
    // the read just yields nothing. The real card on the reference machine
    // does exactly this. An empty attribute reproduces it as the provider
    // sees it, where deleting the file would instead test the different case
    // of a channel that was never discovered at all.
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon0"),
                          QStringLiteral("iwlwifi_1"),
                          {{QStringLiteral("temp1_input"), "38000"},
                           {QStringLiteral("temp2_input"), ""}}));

    HwmonSensorProvider provider(root.path());

    // The channel must still be discovered, so the UI has a row to draw an
    // em dash into rather than silently omitting the sensor.
    const SensorChipInfo *chip = chipById(provider.chips(), QStringLiteral("hwmon0"));
    QVERIFY(chip);
    QCOMPARE(chip->channels.size(), 2);

    QCOMPARE(readingFor(provider.sample(), QStringLiteral("hwmon0"),
                        QStringLiteral("temp1")),
             38.0);
    // -1, never 0: a fake 0 C would be indistinguishable from a real reading
    // and is the exact failure this project's em-dash rule exists to prevent.
    QCOMPARE(readingFor(provider.sample(), QStringLiteral("hwmon0"),
                        QStringLiteral("temp2")),
             -1.0);
}

void TestProviders::sensorsSkipsChipsWithNoReadableChannel()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon0"),
                          QStringLiteral("k10temp"),
                          {{QStringLiteral("temp1_input"), "60125"}}));
    // A hidpp battery registers a hwmon node and publishes nothing this page
    // can plot. An empty group box would be pure noise.
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon1"),
                          QStringLiteral("hidpp_battery_2"),
                          {{QStringLiteral("uevent"), "DEVTYPE=hwmon"}}));

    HwmonSensorProvider provider(root.path());

    QCOMPARE(provider.chips().size(), 1);
    QCOMPARE(provider.chips().first().name, QStringLiteral("k10temp"));
}

void TestProviders::sensorsNumbersOnlyAmbiguousDisplayNames()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon0"),
                          QStringLiteral("k10temp"),
                          {{QStringLiteral("temp1_input"), "60125"}}));
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon1"),
                          QStringLiteral("spd5118"),
                          {{QStringLiteral("temp1_input"), "46250"}}));
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon2"),
                          QStringLiteral("spd5118"),
                          {{QStringLiteral("temp1_input"), "44500"}}));

    HwmonSensorProvider provider(root.path());
    const QVector<SensorChipInfo> chips = provider.chips();

    // Two DIMMs need telling apart; the single CPU chip does not, and "CPU 1"
    // on a machine with one CPU reads like a bug.
    QCOMPARE(chipById(chips, QStringLiteral("hwmon0"))->displayName, QStringLiteral("CPU"));
    QCOMPARE(chipById(chips, QStringLiteral("hwmon1"))->displayName,
             QStringLiteral("Memory module 1"));
    QCOMPARE(chipById(chips, QStringLiteral("hwmon2"))->displayName,
             QStringLiteral("Memory module 2"));
}

void TestProviders::sensorsIgnoresIntrusionAttribute()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon0"),
                          QStringLiteral("nct6799"),
                          {{QStringLiteral("in0_input"), "1225"},
                           {QStringLiteral("intrusion0_alarm"), "0"},
                           {QStringLiteral("intrusion0_beep"), "0"}}));

    HwmonSensorProvider provider(root.path());
    const SensorChipInfo *chip = chipById(provider.chips(), QStringLiteral("hwmon0"));
    QVERIFY(chip);

    // "intrusion0_alarm" starts with "in" but is a chassis switch, not a
    // voltage rail. It is also not an _input, so a pattern that required
    // only the prefix would still have to be wrong twice to admit it — this
    // pins the digit requirement that keeps it out.
    QCOMPARE(chip->channels.size(), 1);
    QCOMPARE(chip->channels.first().id, QStringLiteral("in0"));
}

void TestProviders::sensorsOrdersChipsNumerically()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    for (int i : {0, 2, 10, 11}) {
        QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon%1").arg(i),
                              QStringLiteral("chip%1").arg(i),
                              {{QStringLiteral("temp1_input"), "40000"}}));
    }

    HwmonSensorProvider provider(root.path());
    QStringList ids;
    for (const SensorChipInfo &chip : provider.chips()) {
        ids.append(chip.id);
    }

    // Directory names sort as strings, which puts hwmon10 second. The kernel's
    // registration order is what anyone cross-checking against `sensors` sees.
    QCOMPARE(ids,
             (QStringList{QStringLiteral("hwmon0"), QStringLiteral("hwmon2"),
                          QStringLiteral("hwmon10"), QStringLiteral("hwmon11")}));
}

void TestProviders::sensorsFallsBackToPowerAverage()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    // Some AMD cards publish only an average. Enumerating on `_input` alone
    // would give this chip no power channel at all.
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon0"),
                          QStringLiteral("amdgpu"),
                          {{QStringLiteral("temp1_input"), "44000"},
                           {QStringLiteral("power1_average"), "23000000"}}));

    HwmonSensorProvider provider(root.path());
    const SensorChipInfo *chip = chipById(provider.chips(), QStringLiteral("hwmon0"));
    QVERIFY(chip);
    QVERIFY(channelById(*chip, QStringLiteral("power1")));
    QCOMPARE(readingFor(provider.sample(), QStringLiteral("hwmon0"),
                        QStringLiteral("power1")),
             23.0);
}

void TestProviders::sensorsRecordsParentDevice()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(makeHwmonChip(root.path(), QStringLiteral("hwmon0"),
                          QStringLiteral("amdgpu"),
                          {{QStringLiteral("temp1_input"), "44000"}}));
    const QString pciDir =
        root.path() + QStringLiteral("/sys/devices/pci0000:00/0000:0e:00.0");
    QVERIFY(QDir().mkpath(pciDir));
    QVERIFY(QFile::link(pciDir, root.path()
                                    + QStringLiteral("/sys/class/hwmon/hwmon0/device")));

    HwmonSensorProvider provider(root.path());
    const SensorChipInfo *chip = chipById(provider.chips(), QStringLiteral("hwmon0"));
    QVERIFY(chip);
    // The PCI address is what GPU backends identify a card by, so it is the
    // key that stops a hwmon-visible GPU from being listed a second time.
    QCOMPARE(chip->deviceId, QStringLiteral("0000:0e:00.0"));
}

void TestProviders::gpuSensorsListOnlyCardsHwmonCannotSee()
{
    auto gpu = [](const QString &id, const QString &driver) {
        GpuDeviceInfo info;
        info.id = id;
        info.name = QStringLiteral("Card ") + id;
        info.driver = driver;
        info.metricsAvailable = true;
        return info;
    };
    GpuDeviceInfo unmeasurable = gpu(QStringLiteral("0000:02:00.0"),
                                     QStringLiteral("nvidia"));
    unmeasurable.metricsAvailable = false; // nvidia-utils not installed

    const QVector<GpuDeviceInfo> gpus = {
        gpu(QStringLiteral("0000:01:00.0"), QStringLiteral("nvidia 610.57.04")),
        unmeasurable,
        gpu(QStringLiteral("0000:0e:00.0"), QStringLiteral("amdgpu")),
        // An NVIDIA card that does have a hwmon node, as nouveau would give.
        gpu(QStringLiteral("0000:03:00.0"), QStringLiteral("nvidia")),
    };
    SensorChipInfo hwmonAmd;
    hwmonAmd.id = QStringLiteral("hwmon5");
    hwmonAmd.deviceId = QStringLiteral("0000:0e:00.0");
    SensorChipInfo hwmonNouveau;
    hwmonNouveau.id = QStringLiteral("hwmon6");
    hwmonNouveau.deviceId = QStringLiteral("0000:03:00.0");

    const QVector<SensorChipInfo> chips =
        gpusensors::chips(gpus, {hwmonAmd, hwmonNouveau});

    QCOMPARE(chips.size(), 1);
    QCOMPARE(chips.first().deviceId, QStringLiteral("0000:01:00.0"));
    // Must not collide with an hwmon node id.
    QVERIFY(!chips.first().id.startsWith(QLatin1String("hwmon")));
    QVERIFY(channelById(chips.first(), QStringLiteral("temp1")));
}

void TestProviders::gpuSensorsKeepMeasuredZeroDistinctFromUnknown()
{
    GpuDeviceInfo info;
    info.id = QStringLiteral("0000:01:00.0");
    info.name = QStringLiteral("RTX");
    info.driver = QStringLiteral("nvidia");
    info.metricsAvailable = true;
    const QVector<SensorChipInfo> chips = gpusensors::chips({info}, {});
    QCOMPARE(chips.size(), 1);
    const QString chipId = chips.first().id;

    GpuSnapshot snapshot;
    snapshot.id = info.id;
    snapshot.temperatureC = 46.0;
    snapshot.powerWatts = 36.2;
    snapshot.fanPercent = 0.0; // fans stopped at idle: a real reading

    const QVector<SensorChipSnapshot> live = gpusensors::snapshots({snapshot}, chips);
    QCOMPARE(readingFor(live, chipId, QStringLiteral("temp1")), 46.0);
    QCOMPARE(readingFor(live, chipId, QStringLiteral("power1")), 36.2);
    QCOMPARE(readingFor(live, chipId, QStringLiteral("fan1")), 0.0);

    // No snapshot for the card yet (nvidia-smi lags a tick): every channel is
    // "unreadable", never a zero that would read as a measurement.
    const QVector<SensorChipSnapshot> empty = gpusensors::snapshots({}, chips);
    QCOMPARE(readingFor(empty, chipId, QStringLiteral("temp1")), -1.0);
    QCOMPARE(readingFor(empty, chipId, QStringLiteral("fan1")), -1.0);
}

QTEST_MAIN(TestProviders)
#include "tst_providers.moc"
