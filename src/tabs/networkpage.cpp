#include "networkpage.h"

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

namespace {

// Link speed is reported in Mb/s (decimal megabits), which is how every NIC,
// switch and ISP quotes it — deliberately not converted to the binary units
// the byte counters use.
QString formatLinkSpeed(int megabitsPerSecond)
{
    if (megabitsPerSecond <= 0) {
        return QStringLiteral("—");
    }
    if (megabitsPerSecond >= 1000) {
        return QStringLiteral("%1 Gb/s").arg(megabitsPerSecond / 1000.0, 0, 'g', 3);
    }
    return QStringLiteral("%1 Mb/s").arg(megabitsPerSecond);
}

} // namespace

NetworkPage::NetworkPage(Sampler *sampler, const NetworkDeviceInfo &device,
                         QWidget *parent)
    : QWidget(parent)
    , m_device(device)
{
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

    // Scaling to the link speed would leave ordinary traffic invisible: a
    // browsing session on a 2.5 Gb/s port never leaves the bottom pixel. The
    // axis follows the data instead and labels itself in absolute units, so
    // nothing is hidden by the rescale.
    m_throughputGraph =
        new HistoryGraph(tr("Throughput"), kHistorySeconds, content);
    m_throughputGraph->setAxisFormat(HistoryGraph::AxisFormat::ByteRate);
    m_throughputGraph->setAutoScale(true, 64.0 * 1024.0);
    m_receiveSeries = m_throughputGraph->addSeries(tr("Receive"), QColor());
    m_sendSeries = m_throughputGraph->addSeries(tr("Send"), QColor());
    layout->addWidget(m_throughputGraph, 1, 0, 1, 2);
    layout->setRowStretch(1, 1);

    layout->addWidget(buildDetailsBox(), 2, 0, 1, 2);
    layout->setRowStretch(3, 1);

    auto *scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(scroll);

    applyTheme();

    connect(sampler, &Sampler::networkSampled, this, &NetworkPage::onNetworkSample);
}

void NetworkPage::addDetailRow(QGridLayout *grid, int &row, const QString &key,
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

QWidget *NetworkPage::buildDetailsBox()
{
    auto *box = new QGroupBox(tr("Details"), this);
    auto *grid = new QGridLayout(box);
    grid->setContentsMargins(12, 8, 12, 8);
    grid->setColumnStretch(1, 1);

    int row = 0;
    addDetailRow(grid, row, tr("Interface"), m_device.id);
    addDetailRow(grid, row, tr("Type"), m_device.kind);
    addDetailRow(grid, row, tr("Driver"), m_device.driver);
    addDetailRow(grid, row, tr("Bus address"), m_device.busPath);
    // Live values, refreshed each tick — a cable can be pulled and an address
    // can be handed back by DHCP without the app restarting.
    addDetailRow(grid, row, tr("State"), QStringLiteral("—"), &m_stateValue);
    addDetailRow(grid, row, tr("Link speed"), QStringLiteral("—"),
                 &m_speedValue);
    // One row per address family rather than one packed line: every detail
    // value here has to stay a single short line, because a word-wrapped
    // QLabel is clipped to one line inside a QGridLayout.
    addDetailRow(grid, row, tr("IPv4"), QStringLiteral("—"), &m_ipv4Value);
    addDetailRow(grid, row, tr("IPv6"), QStringLiteral("—"), &m_ipv6Value);
    addDetailRow(grid, row, tr("MAC"), QStringLiteral("—"), &m_macValue);
    addDetailRow(grid, row, tr("Receiving"), QStringLiteral("—"),
                 &m_receiveValue);
    addDetailRow(grid, row, tr("Sending"), QStringLiteral("—"),
                 &m_sendValue);
    addDetailRow(grid, row, tr("Since boot"), QStringLiteral("—"),
                 &m_totalsValue);
    addDetailRow(grid, row, tr("Errors / drops"), QStringLiteral("—"),
                 &m_errorsValue);

    return box;
}

void NetworkPage::applyTheme()
{
    const QColor accent = palette().color(QPalette::Highlight);
    QColor send = accent.toHsv();
    send.setHsv((send.hsvHue() + 180) % 360, send.hsvSaturation(), send.value());

    m_throughputGraph->setSeriesColor(m_receiveSeries, accent);
    m_throughputGraph->setSeriesColor(m_sendSeries, send);

    for (QLabel *label : m_secondaryLabels) {
        theming::markSecondary(label, palette());
    }
}

void NetworkPage::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

void NetworkPage::onNetworkSample(const QVector<NetworkSnapshot> &snapshots)
{
    for (const NetworkSnapshot &snapshot : snapshots) {
        if (snapshot.id != m_device.id) {
            continue;
        }

        m_throughputGraph->pushValue(m_receiveSeries, snapshot.receiveBytesPerSec);
        m_throughputGraph->pushValue(m_sendSeries, snapshot.sendBytesPerSec);

        m_summary->setText(tr("%1 — %2")
                               .arg(m_device.name,
                                    snapshot.up ? tr("connected")
                                                : tr("not connected")));
        m_detail->setText(tr("%1 down · %2 up")
                              .arg(formatting::byteRate(snapshot.receiveBytesPerSec),
                                   formatting::byteRate(snapshot.sendBytesPerSec)));

        QString state = snapshot.state.isEmpty() ? tr("unknown")
                                                 : snapshot.state;
        if (!snapshot.duplex.isEmpty()) {
            state += tr(" · %1 duplex").arg(snapshot.duplex);
        }
        if (snapshot.mtu > 0) {
            state += tr(" · MTU %1").arg(snapshot.mtu);
        }
        m_stateValue->setText(state);

        m_speedValue->setText(formatLinkSpeed(snapshot.linkSpeedMbps));

        // The provider puts IPv4 first, so the split is by content rather than
        // by re-querying: a colon appears in every IPv6 address and in no IPv4
        // one.
        QStringList ipv4;
        QStringList ipv6;
        for (const QString &address : snapshot.addresses) {
            (address.contains(QLatin1Char(':')) ? ipv6 : ipv4).append(address);
        }
        const QString none = QStringLiteral("—");
        m_ipv4Value->setText(ipv4.isEmpty() ? none
                                            : ipv4.join(QStringLiteral(" · ")));
        m_ipv6Value->setText(ipv6.isEmpty() ? none
                                            : ipv6.join(QStringLiteral(" · ")));
        m_macValue->setText(snapshot.macAddress.isEmpty() ? none
                                                          : snapshot.macAddress);

        m_receiveValue->setText(formatting::byteRate(snapshot.receiveBytesPerSec));
        m_sendValue->setText(formatting::byteRate(snapshot.sendBytesPerSec));
        m_totalsValue->setText(
            tr("%1 received · %2 sent")
                .arg(formatting::bytes(snapshot.receiveTotalBytes),
                     formatting::bytes(snapshot.sendTotalBytes)));
        m_errorsValue->setText(
            tr("%1 / %2 in · %3 / %4 out")
                .arg(snapshot.receiveErrors)
                .arg(snapshot.receiveDrops)
                .arg(snapshot.sendErrors)
                .arg(snapshot.sendDrops));
        return;
    }
}
