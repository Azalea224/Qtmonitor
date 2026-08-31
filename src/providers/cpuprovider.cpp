#include "cpuprovider.h"

#include <QFile>
#include <QTextStream>

#include <utility>

// Fields in /proc/stat per-line: user nice system idle iowait irq softirq
// steal guest guest_nice. guest* are already included in user/nice, so they
// are excluded from the total.

ProcfsCpuProvider::ProcfsCpuProvider(QString root)
    : m_root(std::move(root))
{
}

bool ProcfsCpuProvider::isAvailable() const
{
    return QFile::exists(m_root + QStringLiteral("/proc/stat"));
}

bool ProcfsCpuProvider::parseStat(QVector<CoreTimes> &coresOut,
                                  quint64 &ctxtOut, quint64 &intrOut) const
{
    QFile file(m_root + QStringLiteral("/proc/stat"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    coresOut.clear();
    ctxtOut = 0;
    intrOut = 0;

    QTextStream in(&file);
    // Do NOT loop on atEnd(): procfs files report st_size 0, which makes
    // atEnd() return true immediately. Loop until readLine() yields null.
    QString line;
    while (!(line = in.readLine()).isNull()) {
        if (line.startsWith(QLatin1String("cpu"))) {
            const QStringList parts = line.simplified().split(QLatin1Char(' '));
            // parts[0] is the label ("cpu" = aggregate, "cpuN" = core N)
            if (parts.size() < 5) {
                continue;
            }
            quint64 vals[8] = {0};
            const int count = qMin(parts.size() - 1, 8);
            for (int i = 0; i < count; ++i) {
                vals[i] = parts.at(i + 1).toULongLong();
            }
            CoreTimes t;
            t.idle = vals[3] + vals[4]; // idle + iowait
            t.total = vals[0] + vals[1] + vals[2] + vals[3] + vals[4]
                    + vals[5] + vals[6] + vals[7];
            coresOut.append(t);
        } else if (line.startsWith(QLatin1String("ctxt "))) {
            ctxtOut = line.mid(5).simplified().toULongLong();
        } else if (line.startsWith(QLatin1String("intr "))) {
            intrOut = line.mid(5).simplified().split(QLatin1Char(' '))
                          .first().toULongLong();
        }
    }
    return !coresOut.isEmpty();
}

double ProcfsCpuProvider::meanFreqGhzFromCpuinfo() const
{
    QFile file(m_root + QStringLiteral("/proc/cpuinfo"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0.0;
    }
    QTextStream in(&file);
    QString line;
    double sumMhz = 0.0;
    int count = 0;
    while (!(line = in.readLine()).isNull()) {
        if (line.startsWith(QLatin1String("cpu MHz"))) {
            const int colon = line.indexOf(QLatin1Char(':'));
            if (colon >= 0) {
                sumMhz += line.mid(colon + 1).trimmed().toDouble();
                ++count;
            }
        }
    }
    return count > 0 ? sumMhz / count / 1000.0 : 0.0;
}

CpuSnapshot ProcfsCpuProvider::sample()
{
    CpuSnapshot snapshot;

    QVector<CoreTimes> current;
    quint64 ctxt = 0, intr = 0;
    if (!parseStat(current, ctxt, intr)) {
        return snapshot;
    }

    snapshot.currentFreqGhz = meanFreqGhzFromCpuinfo();

    snapshot.contextSwitches = ctxt;
    snapshot.interrupts = intr;
    // coresOut[0] is the aggregate line; the rest are per-core
    snapshot.coreCount = current.size() - 1;

    if (m_havePrevious && m_previous.size() == current.size()) {
        auto usageBetween = [](const CoreTimes &prev, const CoreTimes &cur) {
            const quint64 totalDelta = cur.total - prev.total;
            const quint64 idleDelta = cur.idle - prev.idle;
            if (totalDelta == 0) {
                return 0.0;
            }
            return 100.0 * (1.0 - static_cast<double>(idleDelta) / totalDelta);
        };

        snapshot.totalPercent = usageBetween(m_previous.first(), current.first());
        for (int i = 1; i < current.size(); ++i) {
            snapshot.perCorePercents.append(usageBetween(m_previous.at(i), current.at(i)));
        }
    } else {
        snapshot.perCorePercents.fill(0.0, snapshot.coreCount);
    }

    m_previous = current;
    m_havePrevious = true;
    return snapshot;
}
