#include "memoryprovider.h"

#include <QFile>
#include <QTextStream>

bool ProcfsMemoryProvider::isAvailable() const
{
    return QFile::exists(QStringLiteral("/proc/meminfo"));
}

MemorySnapshot ProcfsMemoryProvider::sample()
{
    MemorySnapshot snapshot;

    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return snapshot;
    }

    quint64 memTotalKb = 0, memAvailableKb = 0, swapTotalKb = 0, swapFreeKb = 0;

    QTextStream in(&file);
    // Do NOT loop on atEnd(): procfs files report st_size 0, which makes
    // atEnd() return true immediately. Loop until readLine() yields null.
    QString line;
    while (!(line = in.readLine()).isNull()) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            continue;
        }
        const QString key = line.left(colon);
        // Values in meminfo are KiB, optionally with a trailing "kB" unit
        const quint64 valueKb = line.mid(colon + 1).simplified()
                                    .split(QLatin1Char(' ')).first().toULongLong();

        if (key == QLatin1String("MemTotal")) {
            memTotalKb = valueKb;
        } else if (key == QLatin1String("MemAvailable")) {
            memAvailableKb = valueKb;
        } else if (key == QLatin1String("SwapTotal")) {
            swapTotalKb = valueKb;
        } else if (key == QLatin1String("SwapFree")) {
            swapFreeKb = valueKb;
        }
    }

    snapshot.memTotalBytes = memTotalKb * 1024;
    snapshot.memAvailableBytes = memAvailableKb * 1024;
    snapshot.memUsedBytes = memTotalKb > memAvailableKb
        ? (memTotalKb - memAvailableKb) * 1024 : 0;
    snapshot.swapTotalBytes = swapTotalKb * 1024;
    snapshot.swapFreeBytes = swapFreeKb * 1024;
    snapshot.swapUsedBytes = swapTotalKb > swapFreeKb
        ? (swapTotalKb - swapFreeKb) * 1024 : 0;
    return snapshot;
}
