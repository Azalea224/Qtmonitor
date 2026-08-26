#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

// A filesystem living on a disk, shown in that disk's details block. Only
// mounts whose source is a real /dev node are tracked: a network or fuse
// mount cannot be attributed to local hardware, and statvfs() on one can
// block for as long as the server takes to answer.
struct DiskMountInfo {
    QString device; // "/dev/nvme0n1p2"
    // Shortest path first. One filesystem can appear at many paths — a btrfs
    // subvolume layout mounts the same device at /, /home, /var/log and more —
    // and listing each as its own row would repeat one set of usage figures
    // seven times over.
    QStringList mountPoints;
    QString filesystem; // "btrfs"
    quint64 totalBytes = 0;
    quint64 usedBytes = 0;
};

// A whole block device as the UI presents it. Identity is the kernel name,
// the one thing /proc/diskstats and /sys/block always agree on.
struct DiskDeviceInfo {
    QString id;   // "nvme0n1"
    QString name; // the model when the device reports one, else the kernel name
    QString kind; // "SSD", "HDD", "Compressed RAM", ...
    QString busPath;
    quint64 sizeBytes = 0;
    bool rotational = false;
    bool removable = false;
};

// Live per-device counters, all derived from /proc/diskstats deltas over the
// measured elapsed time rather than the nominal interval, so a late tick does
// not inflate the rates.
struct DiskSnapshot {
    QString id;
    double readBytesPerSec = 0.0;
    double writeBytesPerSec = 0.0;
    // Share of the interval during which the request queue was non-empty —
    // the kernel's io_ticks. This is what Task Manager calls "active time",
    // and it is deliberately NOT a throughput figure: a disk can sit at 100%
    // active while moving very little, and vice versa.
    double activePercent = 0.0;
    // Mean number of requests in flight across the interval, from the
    // weighted io_ticks field. A queue much deeper than 1 is the honest sign
    // of a disk that is actually the bottleneck.
    double avgQueueLength = 0.0;
    int inFlight = 0;
    QVector<DiskMountInfo> mounts;
};

// Abstract block-device metrics source.
class IDiskProvider
{
public:
    virtual ~IDiskProvider() = default;
    virtual bool isAvailable() const = 0;
    // Enumerated once at construction: hotplugging a disk mid-session is not
    // a case this app tries to follow, for the same reason GPUs are not.
    virtual QVector<DiskDeviceInfo> devices() const = 0;
    virtual QVector<DiskSnapshot> sample() = 0;
};

// Default provider: /proc/diskstats for the counters, /sys/block for the
// hardware description, /proc/self/mounts + statvfs for the filesystems.
class ProcfsDiskProvider final : public IDiskProvider
{
public:
    ProcfsDiskProvider();

    bool isAvailable() const override;
    QVector<DiskDeviceInfo> devices() const override { return m_devices; }
    QVector<DiskSnapshot> sample() override;

private:
    // Raw counters as last read, for the delta.
    struct Counters {
        quint64 sectorsRead = 0;
        quint64 sectorsWritten = 0;
        quint64 ioTicksMsec = 0;
        quint64 weightedIoTicksMsec = 0;
        int inFlight = 0;
        bool valid = false;
    };

    void enumerateDevices();
    void refreshMounts();

    QVector<DiskDeviceInfo> m_devices;
    QHash<QString, Counters> m_previous;
    // Keyed by disk id, so a page only shows the filesystems on its own disk.
    QHash<QString, QVector<DiskMountInfo>> m_mounts;
    QElapsedTimer m_elapsed;
    QElapsedTimer m_sinceMountRefresh;

    // Filesystem usage moves far more slowly than the I/O counters, and each
    // refresh costs a statvfs() per mount — which on a sleeping spinning disk
    // means waking it. Once every few seconds is plenty.
    static constexpr qint64 kMountRefreshMsec = 5000;
};
