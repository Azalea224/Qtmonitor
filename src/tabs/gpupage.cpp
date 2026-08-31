#include "gpupage.h"

#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../formatting.h"
#include "../sampler.h"
#include "../settings.h"
#include "historygraph.h"
#include "theming.h"

namespace {

// Negative readings mean the backend cannot measure that quantity at all.
QString formatOptional(double value, const QString &suffix, int decimals = 0)
{
    if (value < 0.0) {
        return QStringLiteral("—");
    }
    return QStringLiteral("%1%2").arg(value, 0, 'f', decimals).arg(suffix);
}

} // namespace

GpuPage::GpuPage(Sampler *sampler, const GpuDeviceInfo &device, QWidget *parent)
    : QWidget(parent)
    , m_device(device)
{
    // Scroll area for the same reason CpuPage has one: the details block must
    // never be clipped away when the window is short.
    auto *content = new QWidget;
    auto *layout = new QGridLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto summaryFont = font();
    summaryFont.setPointSize(summaryFont.pointSize() + 4);
    summaryFont.setBold(true);

    m_summary = new QLabel(m_device.name, content);
    m_summary->setFont(summaryFont);
    layout->addWidget(m_summary, 0, 0);

    if (!m_device.metricsAvailable) {
        layout->addWidget(buildUnavailableBody(), 1, 0, 1, 2);
        layout->addWidget(buildDetailsBox(), 2, 0, 1, 2);
        layout->setRowStretch(3, 1);
    } else {
        m_detail = new QLabel(content);
        m_secondaryLabels.append(m_detail);
        layout->addWidget(m_detail, 0, 1, Qt::AlignRight);

        m_utilizationGraph =
            new HistoryGraph(tr("GPU utilization"), kHistorySeconds, content);
        m_utilizationGraph->setYUnit(QStringLiteral("%"));
        m_utilizationGraph->setYMax(100.0);
        m_busySeries = m_utilizationGraph->addSeries(tr("GPU"), QColor());
        m_videoSeries = m_utilizationGraph->addSeries(tr("Video"), QColor());
        layout->addWidget(m_utilizationGraph, 1, 0, 1, 2);
        layout->setRowStretch(1, 1);

        int nextRow = 2;
        // Integrated GPUs borrow system RAM and report no VRAM total, so the
        // memory chart only appears where there is a real budget to plot.
        if (m_device.memTotalBytes > 0) {
            m_memoryGraph = new HistoryGraph(tr("GPU memory"),
                                             kHistorySeconds, content);
            const bool smallBudget = m_device.memTotalBytes < 2147483648ull;
            m_memoryDivisor = smallBudget ? 1048576.0 : 1073741824.0;
            m_memoryGraph->setYUnit(smallBudget ? QStringLiteral("MiB")
                                                : QStringLiteral("GiB"));
            m_memoryGraph->setYMax(m_device.memTotalBytes / m_memoryDivisor);
            m_vramSeries = m_memoryGraph->addSeries(tr("VRAM"), QColor());
            layout->addWidget(m_memoryGraph, nextRow, 0, 1, 2);
            layout->setRowStretch(nextRow, 1);
            ++nextRow;

            m_memoryDetail = new QLabel(content);
            m_secondaryLabels.append(m_memoryDetail);
            layout->addWidget(m_memoryDetail, nextRow, 0, 1, 2);
            ++nextRow;
        }

        layout->addWidget(buildDetailsBox(), nextRow, 0, 1, 2);
    }

    auto *scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(scroll);

    applyTheme();

    connect(sampler, &Sampler::gpuSampled, this, &GpuPage::onGpuSample);
}

QWidget *GpuPage::buildUnavailableBody()
{
    auto *holder = new QWidget(this);
    // QVBoxLayout rather than the grid used elsewhere: it honours a wrapped
    // label's height-for-width, which QGridLayout and QFormLayout do not —
    // the same trap that keeps every detail value on a single line.
    auto *layout = new QVBoxLayout(holder);
    layout->setContentsMargins(0, 8, 0, 8);

    auto *hint = new QLabel(m_device.unavailableHint, holder);
    hint->setWordWrap(true);
    hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_secondaryLabels.append(hint);
    layout->addWidget(hint);

    return holder;
}

void GpuPage::addDetailRow(QGridLayout *grid, int &row, const QString &key,
                           const QString &value, QLabel **out)
{
    if (value.isEmpty()) {
        return;
    }
    auto *box = grid->parentWidget();
    auto *keyLabel = new QLabel(key + QLatin1Char(':'), box);
    auto *valueLabel = new QLabel(value, box);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_secondaryLabels.append(valueLabel);
    grid->addWidget(keyLabel, row, 0, Qt::AlignLeft | Qt::AlignTop);
    grid->addWidget(valueLabel, row, 1, Qt::AlignLeft | Qt::AlignTop);
    ++row;
    if (out) {
        *out = valueLabel;
    }
}

QWidget *GpuPage::buildDetailsBox()
{
    auto *box = new QGroupBox(tr("Details"), this);
    // QGridLayout, not QFormLayout, and every value a single short line:
    // wrapped labels get vertically clipped inside both.
    auto *grid = new QGridLayout(box);
    grid->setContentsMargins(12, 8, 12, 8);
    grid->setColumnStretch(1, 1);

    int row = 0;
    addDetailRow(grid, row, tr("Driver"), m_device.driver);
    addDetailRow(grid, row, tr("PCI address"), m_device.id);
    if (m_device.memTotalBytes > 0) {
        addDetailRow(grid, row, tr("Memory"),
                     formatting::bytes(m_device.memTotalBytes));
    }
    if (m_device.metricsAvailable) {
        // Live values, refreshed each tick.
        addDetailRow(grid, row, tr("Temperature"), QStringLiteral("—"),
                     &m_temperatureValue);
        addDetailRow(grid, row, tr("Power draw"), QStringLiteral("—"),
                     &m_powerValue);
        addDetailRow(grid, row, tr("Clock"), QStringLiteral("—"),
                     &m_clockValue);
    }

    return box;
}

void GpuPage::applyTheme()
{
    const QColor accent = palette().color(QPalette::Highlight);
    // Video series: highlight hue rotated 180 degrees, so it contrasts with
    // the utilization line while staying in-palette.
    QColor video = accent.toHsv();
    video.setHsv((video.hsvHue() + 180) % 360, video.hsvSaturation(), video.value());

    if (m_utilizationGraph) {
        m_utilizationGraph->setSeriesColor(m_busySeries, accent);
        m_utilizationGraph->setSeriesColor(m_videoSeries, video);
    }
    if (m_memoryGraph) {
        m_memoryGraph->setSeriesColor(m_vramSeries, accent);
    }

    for (QLabel *label : m_secondaryLabels) {
        theming::markSecondary(label, palette());
    }
}

void GpuPage::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

void GpuPage::onGpuSample(const QVector<GpuSnapshot> &snapshots)
{
    if (!m_device.metricsAvailable) {
        return;
    }

    for (const GpuSnapshot &snapshot : snapshots) {
        if (snapshot.id != m_device.id) {
            continue;
        }

        // A negative reading is "unmeasurable", which must not be plotted as
        // a dip to zero.
        m_utilizationGraph->pushValue(m_busySeries, qMax(0.0, snapshot.busyPercent));
        m_utilizationGraph->pushValue(m_videoSeries, qMax(0.0, snapshot.videoPercent));

        m_summary->setText(tr("%1 — %2")
                               .arg(m_device.name,
                                    formatOptional(snapshot.busyPercent,
                                                   tr("% utilization"), 1)));
        m_detail->setText(
            formatOptional(snapshot.videoPercent, tr("% video engine"), 1));

        if (m_memoryGraph) {
            m_memoryGraph->pushValue(m_vramSeries,
                                     snapshot.memUsedBytes / m_memoryDivisor);
            const quint64 total = snapshot.memTotalBytes > 0 ? snapshot.memTotalBytes
                                                             : m_device.memTotalBytes;
            m_memoryDetail->setText(tr("Memory: %1 / %2")
                                        .arg(formatting::bytes(snapshot.memUsedBytes),
                                             formatting::bytes(total)));
        }

        if (m_temperatureValue) {
            // The only temperature the app shows, so it is the only place the
            // configured unit applies.
            m_temperatureValue->setText(
                snapshot.temperatureC < 0.0
                    ? QStringLiteral("—")
                    : Settings::instance().formatTemperature(snapshot.temperatureC));
        }
        if (m_powerValue) {
            m_powerValue->setText(
                formatOptional(snapshot.powerWatts, QStringLiteral(" W"), 1));
        }
        if (m_clockValue) {
            m_clockValue->setText(
                formatOptional(snapshot.clockMhz, QStringLiteral(" MHz")));
        }
        return;
    }
}
