#include "historygraph.h"

#include <QPainter>
#include <QPainterPath>

#include "../settings.h"

namespace {

// Rounds an axis maximum up to a step a reader can divide in their head. The
// ladder is binary for byte rates (so labels land on 512 KiB/s, not 500) and
// decimal otherwise.
double niceCeiling(double value, bool binary)
{
    const double base = binary ? 1024.0 : 10.0;
    if (!(value > 0.0)) {
        return base;
    }

    double unit = 1.0;
    while (value >= unit * base) {
        unit *= base;
    }
    while (value < unit && unit > 1.0) {
        unit /= base;
    }

    static const double binarySteps[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    static const double decimalSteps[] = {1, 2, 5, 10};
    const double *steps = binary ? binarySteps : decimalSteps;
    const int count = binary ? 11 : 4;

    const double mantissa = value / unit;
    for (int i = 0; i < count; ++i) {
        if (mantissa <= steps[i]) {
            return steps[i] * unit;
        }
    }
    return base * unit;
}

} // namespace

HistoryGraph::HistoryGraph(const QString &title, int historySeconds, QWidget *parent)
    : QWidget(parent)
    , m_title(title)
    , m_historySeconds(historySeconds)
    , m_capacity(Settings::instance().sampleCount(historySeconds))
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    connect(&Settings::instance(), &Settings::pollIntervalChanged, this,
            [this] { applySampleCapacity(); });
}

void HistoryGraph::applySampleCapacity()
{
    m_capacity = Settings::instance().sampleCount(m_historySeconds);
    // A slower speed shortens the buffer, so drop what no longer fits; a
    // faster one just leaves room that fills on the next few ticks.
    for (Series &s : m_series) {
        if (s.values.size() > m_capacity) {
            s.values.remove(m_capacity, s.values.size() - m_capacity);
        }
    }
    update();
}

int HistoryGraph::addSeries(const QString &name, const QColor &color)
{
    m_series.append({name, color, {}});
    return m_series.size() - 1;
}

void HistoryGraph::setSeriesColor(int index, const QColor &color)
{
    if (index >= 0 && index < m_series.size()) {
        m_series[index].color = color;
        update();
    }
}

void HistoryGraph::setYMax(double yMax)
{
    m_yMax = qMax(yMax, 0.001);
    update();
}

void HistoryGraph::setYUnit(const QString &unit)
{
    m_yUnit = unit;
    update();
}

void HistoryGraph::setAxisFormat(AxisFormat format)
{
    m_axisFormat = format;
    update();
}

void HistoryGraph::setAutoScale(bool enabled, double floorValue)
{
    m_autoScale = enabled;
    m_autoScaleFloor = qMax(floorValue, 0.001);
    update();
}

double HistoryGraph::effectiveYMax() const
{
    if (!m_autoScale) {
        return m_yMax;
    }
    double peak = 0.0;
    for (const Series &s : m_series) {
        for (double value : s.values) {
            peak = qMax(peak, value);
        }
    }
    return niceCeiling(qMax(peak, m_autoScaleFloor),
                       m_axisFormat == AxisFormat::ByteRate);
}

void HistoryGraph::pushValue(int seriesIndex, double value)
{
    if (seriesIndex < 0 || seriesIndex >= m_series.size()) {
        return;
    }
    Series &s = m_series[seriesIndex];
    s.values.prepend(value);
    if (s.values.size() > m_capacity) {
        s.values.removeLast();
    }
    update();
}

QSize HistoryGraph::sizeHint() const
{
    return {480, 220};
}

QSize HistoryGraph::minimumSizeHint() const
{
    return {200, 160};
}

QString HistoryGraph::valueLabel(double value, double yMax) const
{
    if (m_axisFormat == AxisFormat::ByteRate) {
        static const char *units[] = {"B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s"};
        // The unit comes from the axis maximum, not from each label's own
        // value, so the gridlines read as one scale instead of drifting from
        // B/s at the bottom to MiB/s at the top.
        int unit = 0;
        double divisor = 1.0;
        while (yMax / divisor >= 1024.0 && unit < 4) {
            divisor *= 1024.0;
            ++unit;
        }
        // Step back down a unit when the maximum is only a small multiple of
        // it. Gridlines fall on quarters of the range, so a 1 MiB/s axis in
        // MiB/s reads 0.0 / 0.3 / 0.5 / 0.8 / 1.0 — two of those five labels
        // rounded. The same axis in KiB/s reads 0 / 256 / 512 / 768 / 1024,
        // every one of them exact, because the auto-scale ladder only ever
        // picks powers of two.
        if (unit > 0 && yMax / divisor < 4.0) {
            divisor /= 1024.0;
            --unit;
        }
        const int decimals = (yMax / divisor) < 10.0 ? 1 : 0;
        return QStringLiteral("%1 %2")
            .arg(value / divisor, 0, 'f', decimals)
            .arg(QLatin1String(units[unit]));
    }

    // Decimals track the axis range: gridlines fall on eighths of it, so a
    // range under 1 lands on steps like 0.125 that one decimal renders as a
    // misleading 0.1 / 0.3 / 0.4 sequence.
    int decimals = 1;
    if (yMax >= 10.0) {
        decimals = 0;
    } else if (yMax < 1.0) {
        decimals = 2;
    }
    const QString number = QString::number(value, 'f', decimals);
    return m_yUnit.isEmpty() ? number
                             : QStringLiteral("%1 %2").arg(number, m_yUnit);
}

void HistoryGraph::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QPalette pal = palette();
    const QColor textColor = pal.color(QPalette::WindowText);
    QColor dimColor = textColor;
    dimColor.setAlpha(160);
    const QColor gridColor = pal.color(QPalette::Mid);

    // With auto-scaling this is derived from the data, so it is computed once
    // per repaint and threaded through the label and plotting maths below.
    const double yMax = effectiveYMax();

    QFont titleFont = painter.font();
    titleFont.setBold(true);
    QFontMetrics titleMetrics(titleFont);
    QFontMetrics axisMetrics(painter.font());

    // Layout: title top-left, legend top-right, Y labels left, X labels bottom
    const int titleHeight = titleMetrics.height() + 8;
    const int yLabelWidth = axisMetrics.horizontalAdvance(valueLabel(yMax, yMax)) + 10;
    const int xLabelHeight = axisMetrics.height() + 6;
    const QRectF plot = QRectF(rect().left() + yLabelWidth,
                               rect().top() + titleHeight,
                               rect().width() - yLabelWidth - 6,
                               rect().height() - titleHeight - xLabelHeight - 2)
                            .adjusted(0, 8, 0, -8); // vertical breathing room

    // Title
    painter.setFont(titleFont);
    painter.setPen(textColor);
    painter.drawText(QRectF(rect().left(), rect().top() + 2,
                            rect().width() / 2, titleMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, m_title);

    // Legend, right-aligned on the title row
    if (!m_series.isEmpty()) {
        int legendX = rect().right() - 4;
        painter.setFont(font());
        for (int i = m_series.size() - 1; i >= 0; --i) {
            const Series &s = m_series.at(i);
            const int nameWidth = axisMetrics.horizontalAdvance(s.name);
            const int swatch = 10;
            legendX -= nameWidth;
            painter.setPen(dimColor);
            painter.drawText(QRect(legendX, rect().top() + 2, nameWidth, titleMetrics.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, s.name);
            legendX -= swatch + 4;
            painter.fillRect(QRectF(legendX, rect().top() + 2 + (titleMetrics.height() - swatch) / 2.0,
                                    swatch, swatch), s.color);
            legendX -= 14; // gap before the previous entry
        }
    }

    // Gridlines + Y labels (0, 25%, 50%, 75%, 100% of yMax)
    painter.setFont(font());
    for (int i = 0; i <= 4; ++i) {
        const double fraction = i / 4.0;
        const double y = plot.bottom() - fraction * plot.height();
        painter.setPen(QPen(gridColor, 1, i == 0 ? Qt::SolidLine : Qt::DotLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(dimColor);
        painter.drawText(QRectF(rect().left(), y - axisMetrics.height() / 2.0,
                                yLabelWidth - 4, axisMetrics.height()),
                         Qt::AlignRight | Qt::AlignVCenter,
                         valueLabel(yMax * fraction, yMax));
    }

    // X labels: oldest on the left, "now" on the right
    painter.setPen(dimColor);
    painter.drawText(QRectF(plot.left(), plot.bottom() + 2, 80, axisMetrics.height()),
                     Qt::AlignLeft | Qt::AlignTop,
                     tr("%1s ago").arg(m_historySeconds));
    painter.drawText(QRectF(plot.right() - 80, plot.bottom() + 2, 80, axisMetrics.height()),
                     Qt::AlignRight | Qt::AlignTop, tr("now"));

    // Series, newest point at the right edge
    const double stepX = plot.width() / (m_capacity - 1.0);
    for (const Series &s : m_series) {
        if (s.values.size() < 2) {
            continue;
        }

        QPainterPath line;
        QPainterPath fill;
        for (int i = 0; i < s.values.size(); ++i) {
            const double clamped = qBound(0.0, s.values.at(i), yMax);
            const double x = plot.right() - i * stepX;
            const double y = plot.bottom() - (clamped / yMax) * plot.height();
            if (i == 0) {
                line.moveTo(x, y);
                fill.moveTo(x, plot.bottom());
                fill.lineTo(x, y);
            } else {
                line.lineTo(x, y);
                fill.lineTo(x, y);
            }
        }
        fill.lineTo(plot.right() - (s.values.size() - 1) * stepX, plot.bottom());
        fill.closeSubpath();

        painter.setClipRect(plot);
        QColor fillColor = s.color;
        fillColor.setAlpha(50);
        painter.fillPath(fill, fillColor);
        painter.setPen(QPen(s.color, 2));
        painter.drawPath(line);
        painter.setClipping(false);
    }

    // Frame
    painter.setPen(QPen(gridColor, 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}
