#include "diskpage.h"

#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../formatting.h"
#include "../sampler.h"
#include "historygraph.h"
#include "theming.h"

DiskPage::DiskPage(Sampler *sampler, const DiskDeviceInfo &device, QWidget *parent)
    : QWidget(parent)
    , m_device(device)
{
    // Same structure as CpuPage and GpuPage: everything inside one scroll
    // area, so the details never get clipped away in a short window.
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

    m_detail = new QLabel(content);
    m_secondaryLabels.append(m_detail);
    layout->addWidget(m_detail, 0, 1, Qt::AlignRight);

    // Throughput has no ceiling to plot against — a disk's rated speed is
    // marketing, and the real limit depends on the access pattern — so the
    // axis follows the data and labels itself in absolute units.
    m_throughputGraph =
        new HistoryGraph(QStringLiteral("Disk throughput"), kHistorySeconds, content);
    m_throughputGraph->setAxisFormat(HistoryGraph::AxisFormat::ByteRate);
    m_throughputGraph->setAutoScale(true, 1024.0 * 1024.0);
    m_readSeries = m_throughputGraph->addSeries(QStringLiteral("Read"), QColor());
    m_writeSeries = m_throughputGraph->addSeries(QStringLiteral("Write"), QColor());
    layout->addWidget(m_throughputGraph, 1, 0, 1, 2);
    layout->setRowStretch(1, 1);

    // Active time is a genuine percentage, so unlike throughput it gets a
    // fixed 0..100 axis. It answers a different question: not how much data
    // moved, but how much of the time the device had work outstanding.
    m_activeGraph =
        new HistoryGraph(QStringLiteral("Active time"), kHistorySeconds, content);
    m_activeGraph->setYUnit(QStringLiteral("%"));
    m_activeGraph->setYMax(100.0);
    m_activeSeries = m_activeGraph->addSeries(QStringLiteral("Active"), QColor());
    layout->addWidget(m_activeGraph, 2, 0, 1, 2);
    layout->setRowStretch(2, 1);

    layout->addWidget(buildDetailsBox(), 3, 0, 1, 2);
    layout->addWidget(buildFilesystemsBox(), 4, 0, 1, 2);

    auto *scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(scroll);

    applyTheme();

    connect(sampler, &Sampler::diskSampled, this, &DiskPage::onDiskSample);
}

void DiskPage::addDetailRow(QGridLayout *grid, int &row, const QString &key,
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

QWidget *DiskPage::buildDetailsBox()
{
    auto *box = new QGroupBox(QStringLiteral("Details"), this);
    // QGridLayout with single-line values: a word-wrapped QLabel gets
    // vertically clipped to one line inside both QGridLayout and QFormLayout.
    auto *grid = new QGridLayout(box);
    grid->setContentsMargins(12, 8, 12, 8);
    grid->setColumnStretch(1, 1);

    int row = 0;
    addDetailRow(grid, row, QStringLiteral("Device"),
                 QStringLiteral("/dev/") + m_device.id);
    addDetailRow(grid, row, QStringLiteral("Type"), m_device.kind);
    addDetailRow(grid, row, QStringLiteral("Capacity"),
                 formatting::bytes(m_device.sizeBytes));
    addDetailRow(grid, row, QStringLiteral("Bus address"), m_device.busPath);
    // Live values, refreshed each tick.
    addDetailRow(grid, row, QStringLiteral("Read speed"), QStringLiteral("—"),
                 &m_readValue);
    addDetailRow(grid, row, QStringLiteral("Write speed"), QStringLiteral("—"),
                 &m_writeValue);
    addDetailRow(grid, row, QStringLiteral("Average queue"), QStringLiteral("—"),
                 &m_queueValue);

    return box;
}

QWidget *DiskPage::buildFilesystemsBox()
{
    m_filesystemsBox = new QGroupBox(QStringLiteral("Filesystems"), this);
    m_filesystemsGrid = new QGridLayout(m_filesystemsBox);
    m_filesystemsGrid->setContentsMargins(12, 8, 12, 8);
    m_filesystemsGrid->setColumnStretch(1, 1);
    // Hidden until the first sample says whether this device has any mounted
    // filesystem at all; a swap partition or a raw disk has none, and an
    // empty box is worse than no box.
    m_filesystemsBox->setVisible(false);
    return m_filesystemsBox;
}

void DiskPage::updateFilesystems(const QVector<DiskMountInfo> &mounts)
{
    QStringList keys;
    keys.reserve(mounts.size());
    for (const DiskMountInfo &mount : mounts) {
        keys.append(mount.mountPoints.join(QLatin1Char(' ')));
    }

    if (keys != m_filesystemKeys) {
        // The set of mount points changed, so the rows are rebuilt. Values are
        // updated in place otherwise, since tearing down a label the user is
        // mid-selection in would drop the selection every few seconds.
        m_filesystemKeys = keys;
        for (QLabel *label : m_filesystemValues) {
            m_secondaryLabels.removeAll(label);
        }
        m_filesystemValues.clear();
        QLayoutItem *item = nullptr;
        while ((item = m_filesystemsGrid->takeAt(0)) != nullptr) {
            if (QWidget *widget = item->widget()) {
                m_secondaryLabels.removeAll(qobject_cast<QLabel *>(widget));
                widget->deleteLater();
            }
            delete item;
        }

        int row = 0;
        for (const DiskMountInfo &mount : mounts) {
            // The extra paths go in the label text as a count and in the
            // tooltip in full: a word-wrapped QLabel is clipped to one line
            // inside a QGridLayout, but a tooltip wraps freely.
            QString key = mount.mountPoints.first();
            if (mount.mountPoints.size() > 1) {
                key += QStringLiteral(" (+%1)").arg(mount.mountPoints.size() - 1);
            }
            auto *keyLabel = new QLabel(key, m_filesystemsBox);
            keyLabel->setToolTip(QStringLiteral("%1 mounted at\n%2")
                                     .arg(mount.device,
                                          mount.mountPoints.join(QLatin1Char('\n'))));
            auto *valueLabel = new QLabel(QStringLiteral("—"), m_filesystemsBox);
            valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            m_secondaryLabels.append(valueLabel);
            m_filesystemValues.append(valueLabel);
            m_filesystemsGrid->addWidget(keyLabel, row, 0,
                                         Qt::AlignLeft | Qt::AlignTop);
            m_filesystemsGrid->addWidget(valueLabel, row, 1,
                                         Qt::AlignLeft | Qt::AlignTop);
            ++row;
        }
        applyTheme();
        m_filesystemsBox->setVisible(!mounts.isEmpty());
    }

    for (int i = 0; i < mounts.size() && i < m_filesystemValues.size(); ++i) {
        const DiskMountInfo &mount = mounts.at(i);
        const double percent = mount.totalBytes > 0
            ? 100.0 * mount.usedBytes / mount.totalBytes
            : 0.0;
        m_filesystemValues.at(i)->setText(
            QStringLiteral("%1 — %2 of %3 used (%4%)")
                .arg(mount.filesystem, formatting::bytes(mount.usedBytes),
                     formatting::bytes(mount.totalBytes),
                     QString::number(percent, 'f', 0)));
    }
}

void DiskPage::applyTheme()
{
    const QColor accent = palette().color(QPalette::Highlight);
    // Write series: the highlight hue rotated 180 degrees, the same trick
    // GpuPage uses to get a second in-palette series color.
    QColor write = accent.toHsv();
    write.setHsv((write.hsvHue() + 180) % 360, write.hsvSaturation(), write.value());

    m_throughputGraph->setSeriesColor(m_readSeries, accent);
    m_throughputGraph->setSeriesColor(m_writeSeries, write);
    m_activeGraph->setSeriesColor(m_activeSeries, accent);

    for (QLabel *label : m_secondaryLabels) {
        theming::markSecondary(label, palette());
    }
}

void DiskPage::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

void DiskPage::onDiskSample(const QVector<DiskSnapshot> &snapshots)
{
    for (const DiskSnapshot &snapshot : snapshots) {
        if (snapshot.id != m_device.id) {
            continue;
        }

        m_throughputGraph->pushValue(m_readSeries, snapshot.readBytesPerSec);
        m_throughputGraph->pushValue(m_writeSeries, snapshot.writeBytesPerSec);
        m_activeGraph->pushValue(m_activeSeries, snapshot.activePercent);

        m_summary->setText(QStringLiteral("%1 — %2% active")
                               .arg(m_device.name,
                                    QString::number(snapshot.activePercent, 'f', 1)));
        m_detail->setText(
            QStringLiteral("%1 read · %2 write")
                .arg(formatting::byteRate(snapshot.readBytesPerSec),
                     formatting::byteRate(snapshot.writeBytesPerSec)));

        m_readValue->setText(formatting::byteRate(snapshot.readBytesPerSec));
        m_writeValue->setText(formatting::byteRate(snapshot.writeBytesPerSec));
        m_queueValue->setText(QStringLiteral("%1 requests")
                                  .arg(snapshot.avgQueueLength, 0, 'f', 2));

        updateFilesystems(snapshot.mounts);
        return;
    }
}
