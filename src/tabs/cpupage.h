#pragma once

#include <QWidget>

#include "../providers/cpuprovider.h"

class QLabel;
class QGridLayout;
class MiniGraph;
class Sampler;

// CPU resource page: overall usage in the header, per-core MiniGraph grid
// as the body.
class CpuPage : public QWidget
{
    Q_OBJECT

public:
    explicit CpuPage(Sampler *sampler, QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;

private:
    void buildCoreGrid(int coreCount);
    QWidget *buildDetailsBox();
    void onCpuSample(const CpuSnapshot &snapshot);

    QLabel *m_summary;
    QLabel *m_detail;
    QLabel *m_governorValue = nullptr;    // refreshed every tick
    QLabel *m_powerPrefValue = nullptr;   // refreshed every tick
    QVector<QLabel *> m_secondaryLabels;

    QVector<MiniGraph *> m_coreGraphs;
    QGridLayout *m_coreGrid;

    static constexpr int kHistorySeconds = 60;
};
