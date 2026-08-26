#pragma once

#include <QVector>
#include <QWidget>

// Lightweight history graph for dense grids (e.g. per-core utilization)
// where a full QChart per cell would be too heavy. Paints a filled polyline
// in the palette Highlight color and re-themes itself automatically.
class MiniGraph : public QWidget
{
    Q_OBJECT

public:
    explicit MiniGraph(int historySeconds, QWidget *parent = nullptr);

    // Values are percentages, 0..100, unless auto-scaling is on.
    void pushValue(double percent);

    // Plots against the largest value currently in the window instead of a
    // fixed 0..100. For throughput there is no meaningful percentage to plot:
    // a 2.5 Gb/s link at ordinary use would be a flat line on the floor. The
    // row's numbers live next to it, so the sparkline only has to carry the
    // shape.
    void setAutoScale(bool enabled);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void applySampleCapacity();

    // As in HistoryGraph: a wall-clock window, converted to a sample count at
    // whatever update speed is configured.
    const int m_historySeconds;
    int m_capacity;
    bool m_autoScale = false;
    QVector<double> m_values; // newest first
};
