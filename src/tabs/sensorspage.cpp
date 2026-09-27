#include "sensorspage.h"

#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

#include "../sampler.h"
#include "../settings.h"
#include "historygraph.h"
#include "theming.h"

namespace {

QString channelKey(const QString &chipId, const QString &channelId)
{
    return chipId + QLatin1Char('/') + channelId;
}

bool hasTemperature(const SensorChipInfo &chip)
{
    for (const SensorChannelInfo &channel : chip.channels) {
        if (channel.kind == SensorChannelInfo::Kind::Temperature) {
            return true;
        }
    }
    return false;
}

} // namespace

SensorsPage::SensorsPage(Sampler *sampler, const QVector<SensorChipInfo> &chips,
                         QWidget *parent)
    : QWidget(parent)
    , m_chips(chips)
{
    auto *content = new QWidget;
    auto *layout = new QGridLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto summaryFont = font();
    summaryFont.setPointSize(summaryFont.pointSize() + 4);
    summaryFont.setBold(true);

    m_summary = new QLabel(tr("Sensors"), content);
    m_summary->setFont(summaryFont);
    layout->addWidget(m_summary, 0, 0);

    m_detail = new QLabel(content);
    m_secondaryLabels.append(m_detail);
    layout->addWidget(m_detail, 0, 1, Qt::AlignRight);

    // One series per chip, plotting that chip's hottest channel, rather than
    // one series per channel. This box has nine temperature channels across
    // six chips: nine lines and a nine-entry legend would be unreadable,
    // where "CPU, GPU, NVMe drive, Memory module 1..." maps onto things a
    // reader recognises. The per-channel detail is in the boxes below.
    m_temperatureGraph =
        new HistoryGraph(tr("Temperatures"), kHistorySeconds, content);
    for (const SensorChipInfo &chip : m_chips) {
        if (hasTemperature(chip)) {
            m_temperatureSeries.insert(
                chip.id, m_temperatureGraph->addSeries(chip.displayName, QColor()));
        }
    }
    // Six series' worth of translucent fill stacks into an opaque block; see
    // HistoryGraph::setFilled. Lines alone read cleanly at this count.
    m_temperatureGraph->setFilled(m_temperatureSeries.size() <= 2);
    layout->addWidget(m_temperatureGraph, 1, 0, 1, 2);
    layout->setRowStretch(1, 1);

    int row = 2;
    for (const SensorChipInfo &chip : m_chips) {
        layout->addWidget(buildChipBox(chip), row++, 0, 1, 2);
    }

    auto *scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(scroll);

    applyTemperatureUnit();
    applyTheme();

    connect(sampler, &Sampler::sensorsSampled, this, &SensorsPage::onSensorsSample);
    // The chart plots whatever unit is configured, and its buffer holds bare
    // numbers, so a switch has to restart the window rather than redraw the
    // old samples against the new axis.
    connect(&Settings::instance(), &Settings::temperatureUnitChanged, this,
            [this] {
                m_temperatureGraph->clearHistory();
                applyTemperatureUnit();
            });
}

void SensorsPage::applyTemperatureUnit()
{
    const bool fahrenheit =
        Settings::instance().temperatureUnit() == Settings::TemperatureUnit::Fahrenheit;
    m_temperatureGraph->setYUnit(fahrenheit ? QStringLiteral("°F")
                                            : QStringLiteral("°C"));
    // A floor rather than a fixed maximum, so a genuinely hot chip can push
    // the axis up instead of being clipped, while an idle machine still gets
    // a stable scale instead of one that rescales down to a 45-degree ceiling
    // and makes 45 degrees look alarming.
    //
    // The Fahrenheit floor is 200, not the boiling point. The auto-scale
    // ladder steps 1/2/5 per decade, so 212 rounds up to 500 and the chart
    // draws its lines in the bottom quarter with labels at 0/125/250/375/500.
    // 200 lands exactly on a step: 0/50/100/150/200. Both units behave the
    // same way above the floor — 100 °C jumps to 200, 200 °F jumps to 500 —
    // and that only happens in territory where somebody is reading the
    // numbers rather than the shape.
    m_temperatureGraph->setAutoScale(true, fahrenheit ? 200.0 : 100.0);
}

QString SensorsPage::formatReading(SensorChannelInfo::Kind kind, double value) const
{
    if (value < 0.0) {
        // The established rule: a channel that cannot be read shows an em
        // dash, never a 0 that would read as a genuine measurement.
        return QStringLiteral("—");
    }
    switch (kind) {
    case SensorChannelInfo::Kind::Temperature:
        return Settings::instance().formatTemperature(value);
    case SensorChannelInfo::Kind::Fan:
        return tr("%1 RPM").arg(value, 0, 'f', 0);
    case SensorChannelInfo::Kind::Voltage:
        return tr("%1 V").arg(value, 0, 'f', 3);
    case SensorChannelInfo::Kind::Power:
        return tr("%1 W").arg(value, 0, 'f', 1);
    case SensorChannelInfo::Kind::Current:
        return tr("%1 A").arg(value, 0, 'f', 2);
    case SensorChannelInfo::Kind::FanDuty:
        return tr("%1 %").arg(value, 0, 'f', 0);
    }
    return {};
}

QWidget *SensorsPage::buildChipBox(const SensorChipInfo &chip)
{
    // The driver name stays visible next to the friendly one. "CPU" is what
    // most people want; "k10temp" is what anyone cross-checking against
    // `sensors` needs, and dropping it would make the two impossible to line
    // up.
    QString title = chip.displayName;
    if (chip.displayName != chip.name) {
        title = tr("%1 · %2").arg(chip.displayName, chip.name);
    }

    auto *box = new QGroupBox(title, this);
    auto *grid = new QGridLayout(box);
    grid->setContentsMargins(12, 8, 12, 8);
    grid->setColumnStretch(2, 1);

    int row = 0;
    if (!chip.alsoShownIn.isEmpty()) {
        auto *note = new QLabel(tr("Also shown on %1").arg(chip.alsoShownIn), box);
        m_secondaryLabels.append(note);
        grid->addWidget(note, row++, 0, 1, 3);
    }

    for (const SensorChannelInfo &channel : chip.channels) {
        auto *nameLabel = new QLabel(channel.label + QLatin1Char(':'), box);
        auto *valueLabel = new QLabel(QStringLiteral("—"), box);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        grid->addWidget(nameLabel, row, 0, Qt::AlignLeft | Qt::AlignTop);
        grid->addWidget(valueLabel, row, 1, Qt::AlignRight | Qt::AlignTop);

        // Limits are genuinely optional — k10temp on this box publishes
        // neither — so the column is simply absent for chips that keep quiet
        // rather than being filled with a placeholder.
        QString limit;
        if (channel.criticalLimit > 0.0) {
            limit = tr("critical at %1")
                        .arg(formatReading(channel.kind, channel.criticalLimit));
        } else if (channel.highLimit > 0.0) {
            limit = tr("high at %1")
                        .arg(formatReading(channel.kind, channel.highLimit));
        }
        if (!limit.isEmpty()) {
            auto *limitLabel = new QLabel(limit, box);
            m_secondaryLabels.append(limitLabel);
            grid->addWidget(limitLabel, row, 2, Qt::AlignLeft | Qt::AlignTop);
        }

        m_valueLabels.insert(channelKey(chip.id, channel.id), valueLabel);
        ++row;
    }

    return box;
}

void SensorsPage::onSensorsSample(const QVector<SensorChipSnapshot> &snapshots)
{
    int readable = 0;
    double hottest = -1.0;

    for (const SensorChipSnapshot &snapshot : snapshots) {
        const auto chipIt =
            std::find_if(m_chips.cbegin(), m_chips.cend(),
                         [&snapshot](const SensorChipInfo &chip) {
                             return chip.id == snapshot.chipId;
                         });
        if (chipIt == m_chips.cend()) {
            continue;
        }

        double chipHottest = -1.0;
        for (const SensorReading &reading : snapshot.readings) {
            const auto channelIt =
                std::find_if(chipIt->channels.cbegin(), chipIt->channels.cend(),
                             [&reading](const SensorChannelInfo &channel) {
                                 return channel.id == reading.channelId;
                             });
            if (channelIt == chipIt->channels.cend()) {
                continue;
            }

            if (QLabel *label =
                    m_valueLabels.value(channelKey(snapshot.chipId, reading.channelId))) {
                label->setText(formatReading(channelIt->kind, reading.value));
            }
            if (reading.value >= 0.0) {
                ++readable;
                if (channelIt->kind == SensorChannelInfo::Kind::Temperature) {
                    chipHottest = qMax(chipHottest, reading.value);
                    hottest = qMax(hottest, reading.value);
                }
            }
        }

        const auto seriesIt = m_temperatureSeries.constFind(snapshot.chipId);
        if (seriesIt != m_temperatureSeries.cend() && chipHottest >= 0.0) {
            m_temperatureGraph->pushValue(
                *seriesIt, Settings::instance().toDisplayTemperature(chipHottest));
        }
    }

    m_detail->setText(hottest >= 0.0
                          ? tr("%1 readings · hottest %2")
                                .arg(readable)
                                .arg(Settings::instance().formatTemperature(hottest))
                          : tr("%1 readings").arg(readable));
}

void SensorsPage::applyTheme()
{
    // GpuPage and DiskPage get a second series colour by rotating the palette
    // highlight 180 degrees. With one series per chip there can be six or
    // more, so the same idea is generalised: spread them evenly around the
    // hue wheel from the accent. Saturation and value are kept, which is what
    // keeps the set looking like it came from the theme rather than from a
    // hard-coded palette that will clash with somebody's desktop.
    //
    // Without this the series keep the invalid QColor they were constructed
    // with and every line paints black — six indistinguishable black lines
    // and a legend of six black swatches, which is exactly how this page
    // first rendered.
    const QColor accent = palette().color(QPalette::Highlight).toHsv();
    const int count = qMax(1, m_temperatureSeries.size());
    int position = 0;
    for (const SensorChipInfo &chip : m_chips) {
        const auto it = m_temperatureSeries.constFind(chip.id);
        if (it == m_temperatureSeries.cend()) {
            continue;
        }
        QColor color;
        color.setHsv((accent.hsvHue() + position * 360 / count) % 360,
                     accent.hsvSaturation(), accent.value());
        m_temperatureGraph->setSeriesColor(*it, color);
        ++position;
    }

    for (QLabel *label : m_secondaryLabels) {
        theming::markSecondary(label, palette());
    }
}

void SensorsPage::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        applyTheme();
    }
}
