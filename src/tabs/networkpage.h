#pragma once

#include <QVector>
#include <QWidget>

#include "../providers/networkprovider.h"

class QGridLayout;
class QLabel;
class HistoryGraph;
class Sampler;

// One resource page per network interface: a throughput chart with separate
// receive and send series, link state, and the addresses currently configured.
class NetworkPage : public QWidget
{
    Q_OBJECT

public:
    NetworkPage(Sampler *sampler, const NetworkDeviceInfo &device,
                QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;

private:
    void applyTheme();
    QWidget *buildDetailsBox();
    void addDetailRow(QGridLayout *grid, int &row, const QString &key,
                      const QString &value, QLabel **out = nullptr);
    void onNetworkSample(const QVector<NetworkSnapshot> &snapshots);

    const NetworkDeviceInfo m_device;

    QLabel *m_summary;
    QLabel *m_detail;
    QLabel *m_stateValue = nullptr;
    QLabel *m_speedValue = nullptr;
    QLabel *m_ipv4Value = nullptr;
    QLabel *m_ipv6Value = nullptr;
    QLabel *m_macValue = nullptr;
    QLabel *m_receiveValue = nullptr;
    QLabel *m_sendValue = nullptr;
    QLabel *m_totalsValue = nullptr;
    QLabel *m_errorsValue = nullptr;
    QVector<QLabel *> m_secondaryLabels;

    HistoryGraph *m_throughputGraph = nullptr;
    int m_receiveSeries = -1;
    int m_sendSeries = -1;

    static constexpr int kHistorySeconds = 60;
};
