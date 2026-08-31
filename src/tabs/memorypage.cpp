#include "memorypage.h"

#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>

#include "../providers/hardwareinfo.h"
#include "../sampler.h"
#include "historygraph.h"
#include "theming.h"

MemoryPage::MemoryPage(Sampler *sampler, QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QGridLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto summaryFont = font();
    summaryFont.setPointSize(summaryFont.pointSize() + 4);
    summaryFont.setBold(true);

    m_summary = new QLabel(tr("Memory"), this);
    m_summary->setFont(summaryFont);
    m_detail = new QLabel(this);
    m_swapDetail = new QLabel(this);
    m_secondaryLabels = {m_detail, m_swapDetail};
    for (QLabel *label : m_secondaryLabels) {
        theming::markSecondary(label, palette());
    }

    m_graph = new HistoryGraph(tr("Memory / swap usage"), kHistorySeconds, this);
    m_graph->setYUnit(QStringLiteral("GiB"));
    m_memSeries = m_graph->addSeries(tr("Memory"), QColor());
    m_swapSeries = m_graph->addSeries(tr("Swap"), QColor());

    layout->addWidget(m_summary, 0, 0);
    layout->addWidget(m_detail, 0, 1, Qt::AlignRight);
    layout->addWidget(m_graph, 1, 0, 1, 2);
    layout->setRowStretch(1, 1); // spare vertical space goes to the chart
    layout->addWidget(m_swapDetail, 2, 0, 1, 2);
    layout->addWidget(buildDetailsBox(), 3, 0, 1, 2);

    applyTheme();

    connect(sampler, &Sampler::memorySampled, this, &MemoryPage::onMemorySample);
}

void MemoryPage::applyTheme()
{
    const QColor accent = palette().color(QPalette::Highlight);
    // Swap series: highlight hue rotated 180 degrees, so it always contrasts
    // with the memory line while staying in-palette.
    QColor swap = accent.toHsv();
    swap.setHsv((swap.hsvHue() + 180) % 360, swap.hsvSaturation(), swap.value());

    m_graph->setSeriesColor(m_memSeries, accent);
    m_graph->setSeriesColor(m_swapSeries, swap);

    for (QLabel *label : m_secondaryLabels) {
        theming::markSecondary(label, palette());
    }
}

void MemoryPage::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

QWidget *MemoryPage::buildDetailsBox()
{
    const MemoryStaticInfo info = loadMemoryStaticInfo();

    auto *box = new QGroupBox(tr("Details"), this);
    // QGridLayout, not QFormLayout: QFormLayout clips word-wrapped labels
    // vertically (broken height-for-width), which truncated long values.
    auto *grid = new QGridLayout(box);
    grid->setContentsMargins(12, 8, 12, 8);
    grid->setColumnStretch(1, 1);

    if (!info.available) {
        auto *hint = new QLabel(
            tr("Memory module details unavailable — the udev database has "
               "no DMI memory properties on this system (needs systemd 255+ "
               "or a boot-time udev trigger)."),
            box);
        hint->setWordWrap(true);
        m_secondaryLabels.append(hint);
        theming::markSecondary(hint, palette());
        grid->addWidget(hint, 0, 0, 1, 2, Qt::AlignLeft | Qt::AlignTop);
        return box;
    }

    int row = 0;
    auto addRow = [this, box, grid, &row](const QString &key, const QString &value) {
        if (value.isEmpty()) {
            return;
        }
        auto *keyLabel = new QLabel(key + QLatin1Char(':'), box);
        auto *valueLabel = new QLabel(value, box);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_secondaryLabels.append(valueLabel);
        theming::markSecondary(valueLabel, palette());
        grid->addWidget(keyLabel, row, 0, Qt::AlignLeft | Qt::AlignTop);
        grid->addWidget(valueLabel, row, 1, Qt::AlignLeft | Qt::AlignTop);
        ++row;
    };

    addRow(tr("Type"), info.type);
    addRow(tr("Configuration"),
           tr("%1 × %2").arg(info.stickCount)
               .arg(formatBytes(info.perStickBytes)));
    if (info.configuredSpeedMTs > 0) {
        // Firmware reports the running speed; if it differs from the rated
        // speed, show both — that usually means XMP/EXPO is off.
        QString speed = tr("%1 MT/s")
                            .arg(info.configuredSpeedMTs, 0, 'f', 0);
        if (info.ratedSpeedMTs > 0
            && !qFuzzyCompare(info.ratedSpeedMTs, info.configuredSpeedMTs)) {
            speed += tr(" (rated %1 MT/s)")
                         .arg(info.ratedSpeedMTs, 0, 'f', 0);
        }
        addRow(tr("Speed"), speed);
    }
    addRow(tr("Manufacturer"), info.manufacturer);
    addRow(tr("Part number"), info.partNumber);
    if (info.ranks > 0) {
        addRow(tr("Ranks"), QString::number(info.ranks));
    }

    return box;
}

QString MemoryPage::formatBytes(quint64 bytes) const
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(units[unit]);
}

void MemoryPage::onMemorySample(const MemorySnapshot &snapshot)
{
    if (!m_axisInitialized && snapshot.memTotalBytes > 0) {
        // Y axis tops out at total RAM; swap values may exceed it and are
        // clipped when drawing.
        m_graph->setYMax(snapshot.memTotalBytes / 1073741824.0);
        m_axisInitialized = true;
    }

    m_graph->pushValue(m_memSeries, snapshot.memUsedBytes / 1073741824.0);
    m_graph->pushValue(m_swapSeries, snapshot.swapUsedBytes / 1073741824.0);

    const double usedPercent = snapshot.memTotalBytes > 0
        ? 100.0 * snapshot.memUsedBytes / snapshot.memTotalBytes
        : 0.0;
    m_summary->setText(tr("Memory — %1 / %2")
                           .arg(formatBytes(snapshot.memUsedBytes),
                                formatBytes(snapshot.memTotalBytes)));
    m_detail->setText(tr("%1% used").arg(usedPercent, 0, 'f', 1));

    if (snapshot.swapTotalBytes > 0) {
        m_swapDetail->setText(tr("Swap: %1 / %2")
                                  .arg(formatBytes(snapshot.swapUsedBytes),
                                       formatBytes(snapshot.swapTotalBytes)));
    } else {
        m_swapDetail->setText(tr("Swap: none configured"));
    }
}
