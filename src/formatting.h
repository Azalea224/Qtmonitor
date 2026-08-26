#pragma once

#include <QLatin1String>
#include <QString>

// Byte and rate formatting shared by the models and the resource pages.
// Binary units throughout, matching what the kernel actually reports.
namespace formatting {

// "512 B", "27.5 MiB", "1.2 TiB". Whole bytes get no decimal, since "27.0 B"
// reads as a measurement precision that does not exist.
inline QString bytes(quint64 value)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double scaled = static_cast<double>(value);
    int unit = 0;
    while (scaled >= 1024.0 && unit < 4) {
        scaled /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2")
        .arg(scaled, 0, 'f', unit == 0 ? 0 : 1)
        .arg(QLatin1String(units[unit]));
}

// Same ladder with a "/s" suffix. Takes a double because throughput is
// derived from a delta over measured elapsed time, not a counter.
inline QString byteRate(double valuePerSecond)
{
    if (valuePerSecond < 1.0) {
        // Below one byte per second is idle for every practical purpose, and
        // "0.4 B/s" is noise dressed up as precision.
        return QStringLiteral("0 B/s");
    }
    return bytes(static_cast<quint64>(valuePerSecond)) + QStringLiteral("/s");
}

} // namespace formatting
