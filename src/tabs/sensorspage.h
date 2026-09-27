#pragma once

#include <QHash>
#include <QVector>
#include <QWidget>

#include "../providers/hwmonprovider.h"

class QGridLayout;
class QLabel;
class HistoryGraph;
class Sampler;

// The Performance tab's Sensors page: every temperature, fan, voltage, power
// and current channel the kernel's hwmon interface exposes.
//
// One page rather than one per chip, which is how disks, GPUs and interfaces
// are handled. A machine here has six chips and would have contributed six
// nav rows for what is really one question ("how hot is this box?"), in a nav
// only 170-220px wide. Chips become group boxes on a single page instead.
class SensorsPage : public QWidget
{
    Q_OBJECT

public:
    SensorsPage(Sampler *sampler, const QVector<SensorChipInfo> &chips,
                QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;

private:
    void applyTheme();
    QWidget *buildChipBox(const SensorChipInfo &chip);
    void onSensorsSample(const QVector<SensorChipSnapshot> &snapshots);
    void applyTemperatureUnit();
    QString formatReading(SensorChannelInfo::Kind kind, double value) const;

    const QVector<SensorChipInfo> m_chips;

    QLabel *m_summary = nullptr;
    QLabel *m_detail = nullptr;
    HistoryGraph *m_temperatureGraph = nullptr;

    // Chart series index per chip id. Only chips with at least one
    // temperature channel get one — a voltage-only chip has nothing to plot
    // on a temperature axis.
    QHash<QString, int> m_temperatureSeries;
    // Value label per "<chipId>/<channelId>", so a sample updates in place
    // rather than rebuilding rows and fighting text selection.
    QHash<QString, QLabel *> m_valueLabels;
    QVector<QLabel *> m_secondaryLabels;

    static constexpr int kHistorySeconds = 60;
};
