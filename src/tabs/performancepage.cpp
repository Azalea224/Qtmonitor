#include "performancepage.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "../sampler.h"
#include "cpupage.h"
#include "diskpage.h"
#include "gpupage.h"
#include "memorypage.h"
#include "minigraph.h"
#include "networkpage.h"
#include "theming.h"

ResourceNavRow::ResourceNavRow(const QString &title, int historySeconds,
                               QListWidget *list)
    : QWidget(list)
    , m_list(list)
{
    if (m_list) {
        m_list->installEventFilter(this);
    }

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(2);

    m_name = new QLabel(title, this);
    m_graph = new MiniGraph(historySeconds, this);
    m_graph->setFixedHeight(40);

    layout->addWidget(m_name);
    layout->addWidget(m_graph);

    applyTheme();
}

void ResourceNavRow::setSelected(bool selected)
{
    if (selected == m_selected) {
        return;
    }
    m_selected = selected;
    applyTheme();
}

void ResourceNavRow::applyTheme()
{
    // Two separate traps here, both measured off screenshots on this box.
    //
    // 1. A label living inside a scroll area's viewport draws in Text, not
    //    WindowText, so both roles have to be set or the selected row keeps
    //    the unselected tint — pale grey on a light Highlight, about 1.2:1.
    // 2. The view paints the *Inactive* Highlight whenever the list does not
    //    have focus, which is how the tab opens. Tinting from the Active
    //    group regardless put the selected label at 1.03:1 against it:
    //    invisible, in the app's default state. legibleOn() keeps the theme's
    //    own color when it works and substitutes black or white when it does
    //    not, so no theme can render this row unreadable.
    const QPalette::ColorGroup group =
        (m_list && m_list->hasFocus()) ? QPalette::Active : QPalette::Inactive;
    const QPalette source = palette();
    const QColor color = m_selected
        ? theming::legibleOn(source.color(group, QPalette::Highlight),
                             source.color(group, QPalette::HighlightedText))
        : source.color(group, QPalette::WindowText);
    QPalette pal = m_name->palette();
    pal.setColor(QPalette::WindowText, color);
    pal.setColor(QPalette::Text, color);
    m_name->setPalette(pal);
}

bool ResourceNavRow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_list
        && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        applyTheme();
    }
    return QWidget::eventFilter(watched, event);
}

void ResourceNavRow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

void ResourceNavRow::pushValue(double percent)
{
    m_graph->pushValue(percent);
}

void ResourceNavRow::setAutoScale(bool enabled)
{
    m_graph->setAutoScale(enabled);
}

PerformancePage::PerformancePage(Sampler *sampler, QWidget *parent)
    : QWidget(parent)
    , m_nav(new QListWidget(this))
    , m_stack(new QStackedWidget(this))
{
    m_cpuRow = addResource(tr("CPU"), ResourceKind::Cpu,
                           new CpuPage(sampler, this));
    m_memRow = addResource(tr("Memory"), ResourceKind::Memory,
                           new MemoryPage(sampler, this));
    addGpuResources(sampler);
    addDiskResources(sampler);
    addNetworkResources(sampler);

    m_nav->setSpacing(2);
    m_nav->setUniformItemSizes(true);
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->setMinimumWidth(170);
    m_nav->setMaximumWidth(220);
    m_nav->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    connect(m_nav, &QListWidget::currentRowChanged, this, [this](int index) {
        m_stack->setCurrentIndex(index);
        for (int row = 0; row < m_rows.size(); ++row) {
            m_rows.at(row)->setSelected(row == index);
        }
    });
    m_nav->setCurrentRow(0);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_nav);
    layout->addWidget(m_stack, 1);

    connect(sampler, &Sampler::cpuSampled, this, &PerformancePage::onCpuSample);
    connect(sampler, &Sampler::memorySampled, this, &PerformancePage::onMemorySample);
    connect(sampler, &Sampler::gpuSampled, this, &PerformancePage::onGpuSample);
    connect(sampler, &Sampler::diskSampled, this, &PerformancePage::onDiskSample);
    connect(sampler, &Sampler::networkSampled, this, &PerformancePage::onNetworkSample);
}

void PerformancePage::addGpuResources(Sampler *sampler)
{
    const QVector<GpuDeviceInfo> gpus = sampler->gpus().devices();
    for (int i = 0; i < gpus.size(); ++i) {
        const GpuDeviceInfo &device = gpus.at(i);
        // The sidebar is 170-220px wide, so the row carries a short label and
        // the page header carries the model name. Numbering only appears when
        // there is more than one card to tell apart.
        const QString title = gpus.size() > 1 ? tr("GPU %1").arg(i)
                                              : tr("GPU");
        m_gpuRows.insert(device.id,
                         addResource(title, ResourceKind::Gpu,
                                     new GpuPage(sampler, device, this)));
    }
}

void PerformancePage::addDiskResources(Sampler *sampler)
{
    const QVector<DiskDeviceInfo> disks = sampler->disks();
    for (const DiskDeviceInfo &device : disks) {
        // The kernel name, not the model: "nvme0n1" is short enough for a
        // 170px column and is what every other Linux tool calls the device.
        // The model goes in the page header, as with GPUs.
        m_diskRows.insert(device.id,
                          addResource(device.id, ResourceKind::Disk,
                                      new DiskPage(sampler, device, this)));
    }
}

void PerformancePage::addNetworkResources(Sampler *sampler)
{
    const QVector<NetworkDeviceInfo> interfaces = sampler->networkInterfaces();
    for (const NetworkDeviceInfo &device : interfaces) {
        ResourceNavRow *row = addResource(device.id, ResourceKind::Network,
                                          new NetworkPage(sampler, device, this));
        // Throughput has no ceiling to be a percentage of.
        row->setAutoScale(true);
        m_networkRows.insert(device.id, row);
    }
}

ResourceNavRow *PerformancePage::addResource(const QString &title, ResourceKind kind,
                                             QWidget *page)
{
    Q_UNUSED(kind);
    auto *item = new QListWidgetItem(m_nav);
    auto *row = new ResourceNavRow(title, kHistorySeconds, m_nav);
    item->setSizeHint(row->sizeHint());
    m_nav->setItemWidget(item, row);
    m_stack->addWidget(page);
    m_rows.append(row);
    return row;
}

void PerformancePage::onCpuSample(const CpuSnapshot &snapshot)
{
    m_cpuRow->pushValue(snapshot.totalPercent);
}

void PerformancePage::onMemorySample(const MemorySnapshot &snapshot)
{
    const double usedPercent = snapshot.memTotalBytes > 0
        ? 100.0 * snapshot.memUsedBytes / snapshot.memTotalBytes
        : 0.0;
    m_memRow->pushValue(usedPercent);
}

void PerformancePage::onDiskSample(const QVector<DiskSnapshot> &snapshots)
{
    // Active time, not throughput: it is already a percentage, and it is the
    // figure that says whether the disk is the thing holding the machine up.
    for (const DiskSnapshot &snapshot : snapshots) {
        if (ResourceNavRow *row = m_diskRows.value(snapshot.id)) {
            row->pushValue(snapshot.activePercent);
        }
    }
}

void PerformancePage::onNetworkSample(const QVector<NetworkSnapshot> &snapshots)
{
    // Combined traffic in both directions, on an auto-scaled sparkline: one
    // line has to stand for the whole interface here, and the page splits it
    // into receive and send.
    for (const NetworkSnapshot &snapshot : snapshots) {
        if (ResourceNavRow *row = m_networkRows.value(snapshot.id)) {
            row->pushValue(snapshot.receiveBytesPerSec + snapshot.sendBytesPerSec);
        }
    }
}

void PerformancePage::onGpuSample(const QVector<GpuSnapshot> &snapshots)
{
    // Cards nothing can measure have a row but never a sample, so their
    // sparkline stays flat rather than showing invented activity.
    for (const GpuSnapshot &snapshot : snapshots) {
        if (ResourceNavRow *row = m_gpuRows.value(snapshot.id)) {
            row->pushValue(qMax(0.0, snapshot.busyPercent));
        }
    }
}
