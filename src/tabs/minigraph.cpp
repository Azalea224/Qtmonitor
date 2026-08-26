#include "minigraph.h"

#include <QPainter>
#include <QPainterPath>

#include "../settings.h"

MiniGraph::MiniGraph(int historySeconds, QWidget *parent)
    : QWidget(parent)
    , m_historySeconds(historySeconds)
    , m_capacity(Settings::instance().sampleCount(historySeconds))
{
    // Fill available horizontal space like the other graphs; height stays
    // fixed so grid rows keep a uniform, square-ish feel.
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    connect(&Settings::instance(), &Settings::pollIntervalChanged, this,
            [this] { applySampleCapacity(); });
}

void MiniGraph::applySampleCapacity()
{
    m_capacity = Settings::instance().sampleCount(m_historySeconds);
    if (m_values.size() > m_capacity) {
        m_values.remove(m_capacity, m_values.size() - m_capacity);
    }
    update();
}

void MiniGraph::setAutoScale(bool enabled)
{
    if (m_autoScale == enabled) {
        return;
    }
    m_autoScale = enabled;
    // The stored values mean something different in each mode, so anything
    // already buffered would plot as nonsense at the new scale.
    m_values.clear();
    update();
}

void MiniGraph::pushValue(double percent)
{
    m_values.prepend(m_autoScale ? qMax(0.0, percent) : qBound(0.0, percent, 100.0));
    if (m_values.size() > m_capacity) {
        m_values.removeLast();
    }
    update();
}

QSize MiniGraph::sizeHint() const
{
    return {96, 64};
}

QSize MiniGraph::minimumSizeHint() const
{
    return {64, 48};
}

void MiniGraph::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF area = rect().adjusted(1, 1, -1, -1);
    const QColor base = palette().color(QPalette::Base);
    const QColor accent = palette().color(QPalette::Highlight);

    painter.fillRect(rect(), base);

    if (m_values.size() >= 2) {
        const double stepX = area.width() / (m_capacity - 1.0);
        // A flat-zero window must not divide by zero, and must stay flat
        // rather than snapping to full height.
        double scale = 100.0;
        if (m_autoScale) {
            scale = 0.0;
            for (double value : m_values) {
                scale = qMax(scale, value);
            }
            if (scale <= 0.0) {
                scale = 1.0;
            }
        }

        QPainterPath line;
        QPainterPath fill;
        for (int i = 0; i < m_values.size(); ++i) {
            const double x = area.right() - i * stepX;
            const double y = area.bottom() - (m_values.at(i) / scale) * area.height();
            if (i == 0) {
                line.moveTo(x, y);
                fill.moveTo(x, area.bottom());
                fill.lineTo(x, y);
            } else {
                line.lineTo(x, y);
                fill.lineTo(x, y);
            }
        }
        fill.lineTo(area.right() - (m_values.size() - 1) * stepX, area.bottom());
        fill.closeSubpath();

        QColor fillColor = accent;
        fillColor.setAlpha(60);
        painter.fillPath(fill, fillColor);

        QPen pen(accent);
        pen.setWidthF(1.5);
        painter.setPen(pen);
        painter.drawPath(line);
    }

    painter.setPen(QPen(palette().color(QPalette::Mid), 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}
