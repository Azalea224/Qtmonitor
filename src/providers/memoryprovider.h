#pragma once

#include <QString>
#include <QtGlobal>

struct MemorySnapshot {
    quint64 memTotalBytes = 0;
    quint64 memAvailableBytes = 0; // MemAvailable: estimate of usable memory
    quint64 memUsedBytes = 0;      // total - available
    quint64 swapTotalBytes = 0;
    quint64 swapFreeBytes = 0;
    quint64 swapUsedBytes = 0;
};

// Abstract memory metrics source.
class IMemoryProvider
{
public:
    virtual ~IMemoryProvider() = default;
    virtual bool isAvailable() const = 0;
    virtual MemorySnapshot sample() = 0;
};

// Default provider: reads /proc/meminfo.
class ProcfsMemoryProvider final : public IMemoryProvider
{
public:
    // See ProcfsCpuProvider: empty root means the real /proc.
    explicit ProcfsMemoryProvider(QString root = QString());

    bool isAvailable() const override;
    MemorySnapshot sample() override;

private:
    QString m_root;
};
