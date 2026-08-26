#pragma once

#include <QString>
#include <QVector>

struct CpuSnapshot {
    double totalPercent = 0.0;
    QVector<double> perCorePercents;
    int coreCount = 0;
    quint64 contextSwitches = 0;
    quint64 interrupts = 0;
    double currentFreqGhz = 0.0; // mean of /proc/cpuinfo "cpu MHz"
};

// Abstract CPU metrics source. Implementations must be cheap to call at a
// ~1s cadence and never block on I/O beyond reading /proc or /sys.
class ICpuProvider
{
public:
    virtual ~ICpuProvider() = default;
    virtual bool isAvailable() const = 0;
    virtual CpuSnapshot sample() = 0;
};

// Default provider: reads /proc/stat and computes utilization from the
// delta between consecutive samples. The first sample after construction
// reports 0% since no delta exists yet.
class ProcfsCpuProvider final : public ICpuProvider
{
public:
    bool isAvailable() const override;
    CpuSnapshot sample() override;

private:
    struct CoreTimes {
        quint64 idle = 0;
        quint64 total = 0;
    };

    static bool parseStat(QVector<CoreTimes> &coresOut,
                          quint64 &ctxtOut, quint64 &intrOut);

    QVector<CoreTimes> m_previous;
    bool m_havePrevious = false;
};
