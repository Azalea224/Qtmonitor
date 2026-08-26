#pragma once

#include <QWidget>

#include "../providers/memoryprovider.h"

class QLabel;
class HistoryGraph;
class Sampler;

// Memory resource page: one HistoryGraph with a memory series and a
// differently coloured swap series, plus summary labels.
class MemoryPage : public QWidget
{
    Q_OBJECT

public:
    explicit MemoryPage(Sampler *sampler, QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;

private:
    void applyTheme();
    QWidget *buildDetailsBox();
    QString formatBytes(quint64 bytes) const;
    void onMemorySample(const MemorySnapshot &snapshot);

    HistoryGraph *m_graph;
    int m_memSeries;
    int m_swapSeries;
    bool m_axisInitialized = false;

    QLabel *m_summary;
    QLabel *m_detail;
    QLabel *m_swapDetail;
    QVector<QLabel *> m_secondaryLabels;

    static constexpr int kHistorySeconds = 60;
};
