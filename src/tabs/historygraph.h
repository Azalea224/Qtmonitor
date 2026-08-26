#pragma once

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

// Custom-painted rolling history chart. Supports multiple named series,
// horizontal gridlines with Y-axis value labels, an X axis marked in
// "seconds ago", a title, and a legend. All colors come from the widget
// palette, so it follows DE theme changes with no extra work.
class HistoryGraph : public QWidget
{
    Q_OBJECT

public:
    explicit HistoryGraph(const QString &title, int historySeconds,
                          QWidget *parent = nullptr);

    // Adds a series, returns its index for pushValue().
    int addSeries(const QString &name, const QColor &color);
    void setSeriesColor(int index, const QColor &color);

    // How the Y-axis labels are rendered.
    enum class AxisFormat {
        Number,   // the value plus whatever setYUnit() was given
        ByteRate, // values are bytes per second; the axis picks its own
                  // binary unit from the current range and labels every
                  // gridline in it
    };

    // Y axis runs 0..yMax; values above are clipped when drawing.
    void setYMax(double yMax);
    void setYUnit(const QString &unit); // e.g. "GiB", "%" — appended to labels
    void setAxisFormat(AxisFormat format);

    // Rescales the axis on every repaint to the largest value currently in
    // the window, rounded up to a readable step and never below `floorValue`.
    //
    // Throughput has no natural maximum the way a percentage does: pinning a
    // network chart to the link speed leaves ordinary traffic an invisible
    // sliver against a 2.5 Gb/s axis. Because the labelled axis moves with
    // the data, the reader still sees absolute numbers — unlike a sparkline,
    // nothing is hidden by the rescale.
    void setAutoScale(bool enabled, double floorValue = 1.0);

    void pushValue(int seriesIndex, double value);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Series {
        QString name;
        QColor color;
        QVector<double> values; // newest first
    };

    // Both take the axis maximum explicitly, because with auto-scaling it is
    // computed per repaint rather than stored.
    QString valueLabel(double value, double yMax) const;
    double effectiveYMax() const;

    void applySampleCapacity();

    const QString m_title;
    // The window is wall-clock seconds; how many samples that is depends on
    // the configured update speed, so it is recomputed when that changes.
    const int m_historySeconds;
    int m_capacity;
    double m_yMax = 1.0;
    QString m_yUnit;
    AxisFormat m_axisFormat = AxisFormat::Number;
    bool m_autoScale = false;
    double m_autoScaleFloor = 1.0;
    QVector<Series> m_series;
};
