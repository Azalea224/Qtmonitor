#pragma once

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

// Static (or slow-changing) hardware details, loaded once at page
// construction. All sources are best-effort; anything missing is simply
// left empty/zero and the UI hides those rows.

struct CpuStaticInfo {
    QString modelName;        // /proc/cpuinfo "model name"
    double maxFreqGhz = 0.0;  // cpufreq cpuinfo_max_freq
    QString driver;           // cpufreq scaling_driver
    QString governor;         // cpufreq scaling_governor
    QString energyPreference; // energy_performance_preference (may be empty)
    // (label, size) pairs, e.g. ("L1d", "48 KiB") — one UI row each so no
    // single label needs word wrapping.
    QVector<QPair<QString, QString>> caches;
    QStringList instructionSets; // curated, present-only, e.g. "AVX2"
};

struct MemoryStaticInfo {
    bool available = false;
    int stickCount = 0;
    quint64 perStickBytes = 0;
    double configuredSpeedMTs = 0.0; // speed the modules actually run at
    double ratedSpeedMTs = 0.0;      // SMBIOS "Speed" field
    QString type;                    // DDR4, DDR5, ...
    QString manufacturer;            // resolved, incl. JEDEC ID decode
    QString partNumber;
    int ranks = 0;
};

// Fast-changing fields re-read on every poll tick.
struct CpuDynamicInfo {
    QString governor;
    QString energyPreference;
};

CpuStaticInfo loadCpuStaticInfo();
CpuDynamicInfo loadCpuDynamicInfo();
MemoryStaticInfo loadMemoryStaticInfo();
