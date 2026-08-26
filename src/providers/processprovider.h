#pragma once

#include <QHash>
#include <QString>
#include <QVector>

struct ProcessInfo {
    int pid = 0;
    int ppid = 0;
    QString name;    // comm, without the surrounding parens
    QString command; // full cmdline, empty for kernel threads
    QString user;    // resolved from the owning uid
    uint uid = 0;
    // Share of total CPU capacity, Task Manager style: one saturated thread
    // on a 32-thread machine reads ~3%, not 100%.
    double cpuPercent = 0.0;
    quint64 rssBytes = 0;
    QChar state; // R running, S sleeping, D uninterruptible, Z zombie, ...
    int nice = 0;
    int threads = 0;
    // /proc/PID/stat field 22. Combined with the pid this identifies a
    // process exactly, which the privileged helper relies on to detect PID
    // reuse before signalling anything.
    quint64 startTicks = 0;
    quint64 diskReadBytesPerSec = 0;
    quint64 diskWriteBytesPerSec = 0;
    // /proc/PID/io is only readable for our own processes (or as root), so
    // the disk columns are legitimately unknown for other users' processes.
    bool ioAccessible = false;
    bool isKernelThread = false;
    // Phase 4 (GPU provider) fills these; negative means "not measured".
    double gpuComputePercent = -1.0;
    double gpuVideoPercent = -1.0;
};

struct ProcessSnapshot {
    QVector<ProcessInfo> processes; // ascending by pid
    int kernelThreadCount = 0;
};

// Abstract process table source.
class IProcessProvider
{
public:
    virtual ~IProcessProvider() = default;
    virtual bool isAvailable() const = 0;
    virtual ProcessSnapshot sample() = 0;
};

// Default provider: walks /proc/[0-9]* each tick. CPU and disk figures are
// deltas against the previous sample, so the first sample reports zeroes.
class ProcfsProcessProvider final : public IProcessProvider
{
public:
    bool isAvailable() const override;
    ProcessSnapshot sample() override;

private:
    // Per-process counters carried between samples to derive rates.
    struct PrevCounters {
        quint64 cpuTicks = 0;
        quint64 readBytes = 0;
        quint64 writeBytes = 0;
        quint64 startTicks = 0; // guards against PID reuse
    };

    // cmdline and owner never change over a process's life, so they are read
    // once and reused. Keyed by pid, validated by startTicks.
    struct CachedStatics {
        QString command;
        QString user;
        uint uid = 0;
        bool isKernelThread = false;
        quint64 startTicks = 0;
    };

    // Returned by value: a pointer into the hash would dangle the moment a
    // later insert rehashes it.
    CachedStatics staticsFor(int pid, quint64 startTicks);
    QString userNameFor(uint uid);

    QHash<int, PrevCounters> m_previous;
    QHash<int, CachedStatics> m_statics;
    QHash<uint, QString> m_userNames;
    quint64 m_previousTotalTicks = 0;
    bool m_havePrevious = false;
    qint64 m_previousSampleMsec = 0;
};
