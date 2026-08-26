#pragma once

#include <QColor>
#include <QLabel>
#include <QPalette>

#include <cmath>

// Shared palette helpers. Secondary/dimmed text: NEVER use QPalette::Dark
// (near-black, invisible on dark themes) — derive the tint from WindowText
// at reduced alpha, legible on any theme.
namespace theming {

inline QColor secondaryTextColor(const QPalette &pal)
{
    QColor color = pal.color(QPalette::WindowText);
    color.setAlpha(160); // ~63% opacity: clearly dimmed, still readable
    return color;
}

// WCAG relative luminance, and the contrast ratio built from it. Used to
// check a palette's own choices rather than trusting them: a theme is free to
// hand back a HighlightedText that does not contrast with the Highlight the
// view actually paints.
inline double relativeLuminance(const QColor &color)
{
    auto channel = [](double value) {
        value /= 255.0;
        return value <= 0.03928 ? value / 12.92
                                : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.red()) + 0.7152 * channel(color.green())
        + 0.0722 * channel(color.blue());
}

inline double contrastRatio(const QColor &a, const QColor &b)
{
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

// Returns `preferred` when it is actually readable on `background`, and plain
// black or white otherwise. Respects the theme's intent where the theme is
// right, and refuses to render invisible text where it is not.
inline QColor legibleOn(const QColor &background, const QColor &preferred,
                        double minimumRatio = 3.0)
{
    if (contrastRatio(background, preferred) >= minimumRatio) {
        return preferred;
    }
    return relativeLuminance(background) > 0.18 ? QColor(Qt::black) : QColor(Qt::white);
}

// Applies the secondary tint to a label. The label keeps a copied palette,
// so call again (or re-tint) on PaletteChange.
inline void markSecondary(QLabel *label, const QPalette &pal)
{
    QPalette labelPal = label->palette();
    labelPal.setColor(QPalette::WindowText, secondaryTextColor(pal));
    label->setPalette(labelPal);
}

} // namespace theming
