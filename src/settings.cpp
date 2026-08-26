#include "settings.h"

#include <QApplication>
#include <QPalette>
#include <QStyleHints>

#include <KConfigGroup>
#include <KSharedConfig>

#include <algorithm>

namespace {

constexpr const char *kGroup = "General";

// Below a quarter second the sampling costs more than the readings are worth;
// above ten the graphs stop being a live view of anything.
constexpr int kMinIntervalMsec = 250;
constexpr int kMaxIntervalMsec = 10000;

KConfigGroup configGroup()
{
    return KConfigGroup(KSharedConfig::openConfig(), QLatin1String(kGroup));
}

// Both overrides are spelled out rather than derived from a style, because a
// style's idea of "standard" is whatever its own theme happens to be. These
// are plain, neutral schemes that any Qt style can draw.
QPalette buildPalette(bool dark)
{
    const QColor window = dark ? QColor(0x35, 0x35, 0x35) : QColor(0xef, 0xef, 0xef);
    const QColor text = dark ? QColor(0xff, 0xff, 0xff) : QColor(0x00, 0x00, 0x00);
    const QColor base = dark ? QColor(0x2a, 0x2a, 0x2a) : QColor(0xff, 0xff, 0xff);
    const QColor alternate = dark ? QColor(0x3a, 0x3a, 0x3a) : QColor(0xf7, 0xf7, 0xf7);
    const QColor highlight = dark ? QColor(0x2a, 0x82, 0xda) : QColor(0x30, 0x8c, 0xc6);
    const QColor disabled = dark ? QColor(0x7f, 0x7f, 0x7f) : QColor(0xa0, 0xa0, 0xa0);

    QPalette palette;
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, alternate);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::ToolTipBase, window);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Button, window);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor(0xff, 0x40, 0x40));
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText,
                     dark ? QColor(0x00, 0x00, 0x00) : QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::Link, highlight);
    palette.setColor(QPalette::LinkVisited, highlight.darker(120));

    QColor placeholder = text;
    placeholder.setAlpha(128);
    palette.setColor(QPalette::PlaceholderText, placeholder);

    for (const QPalette::ColorRole role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
          QPalette::HighlightedText}) {
        palette.setColor(QPalette::Disabled, role, disabled);
    }
    palette.setColor(QPalette::Disabled, QPalette::Highlight,
                     dark ? QColor(0x50, 0x50, 0x50) : QColor(0xd0, 0xd0, 0xd0));
    return palette;
}

} // namespace

Settings::Settings()
    : m_systemPalette(QApplication::palette())
{
    const KConfigGroup group = configGroup();
    m_pollIntervalMsec = std::clamp(group.readEntry("pollIntervalMsec", 1000),
                                    kMinIntervalMsec, kMaxIntervalMsec);
    m_temperatureUnit = group.readEntry("temperatureUnit", QString()) == QLatin1String("F")
        ? TemperatureUnit::Fahrenheit
        : TemperatureUnit::Celsius;

    const QString theme = group.readEntry("theme", QString());
    if (theme == QLatin1String("light")) {
        m_theme = Theme::Light;
    } else if (theme == QLatin1String("dark")) {
        m_theme = Theme::Dark;
    } else {
        m_theme = Theme::System;
    }

    m_showKernelThreads = group.readEntry("showKernelThreads", false);
}

Settings &Settings::instance()
{
    static Settings settings;
    return settings;
}

void Settings::setPollIntervalMsec(int msec)
{
    const int clamped = std::clamp(msec, kMinIntervalMsec, kMaxIntervalMsec);
    if (clamped == m_pollIntervalMsec) {
        return;
    }
    m_pollIntervalMsec = clamped;

    KConfigGroup group = configGroup();
    group.writeEntry("pollIntervalMsec", clamped);
    group.sync();

    emit pollIntervalChanged(clamped);
}

void Settings::setTemperatureUnit(TemperatureUnit unit)
{
    if (unit == m_temperatureUnit) {
        return;
    }
    m_temperatureUnit = unit;

    KConfigGroup group = configGroup();
    group.writeEntry("temperatureUnit",
                     unit == TemperatureUnit::Fahrenheit ? QStringLiteral("F")
                                                         : QStringLiteral("C"));
    group.sync();

    emit temperatureUnitChanged();
}

void Settings::setTheme(Theme theme)
{
    if (theme == m_theme) {
        return;
    }
    m_theme = theme;

    KConfigGroup group = configGroup();
    switch (theme) {
    case Theme::Light:
        group.writeEntry("theme", QStringLiteral("light"));
        break;
    case Theme::Dark:
        group.writeEntry("theme", QStringLiteral("dark"));
        break;
    case Theme::System:
        group.writeEntry("theme", QStringLiteral("system"));
        break;
    }
    group.sync();

    applyTheme();
}

void Settings::setShowKernelThreads(bool show)
{
    if (show == m_showKernelThreads) {
        return;
    }
    m_showKernelThreads = show;

    KConfigGroup group = configGroup();
    group.writeEntry("showKernelThreads", show);
    group.sync();

    emit showKernelThreadsChanged(show);
}

void Settings::applyTheme() const
{
    QStyleHints *hints = QApplication::styleHints();
    if (!hints) {
        return;
    }
    switch (m_theme) {
    case Theme::Light:
        hints->setColorScheme(Qt::ColorScheme::Light);
        QApplication::setPalette(buildPalette(false));
        break;
    case Theme::Dark:
        hints->setColorScheme(Qt::ColorScheme::Dark);
        QApplication::setPalette(buildPalette(true));
        break;
    case Theme::System:
        // Hands the choice back to the platform theme, which is the default
        // and the only mode that follows a desktop-wide light/dark switch.
        hints->unsetColorScheme();
        QApplication::setPalette(m_systemPalette);
        break;
    }
}

QString Settings::formatTemperature(double celsius) const
{
    if (m_temperatureUnit == TemperatureUnit::Fahrenheit) {
        return QStringLiteral("%1 °F").arg(celsius * 9.0 / 5.0 + 32.0, 0, 'f', 0);
    }
    return QStringLiteral("%1 °C").arg(celsius, 0, 'f', 0);
}

int Settings::sampleCount(int seconds) const
{
    // Two points are the fewest that can draw a line.
    return std::max(2, seconds * 1000 / m_pollIntervalMsec);
}

QVector<int> Settings::pollIntervalChoices()
{
    return {500, 1000, 2000, 5000};
}

QString Settings::describePollInterval(int msec)
{
    if (msec % 1000 == 0) {
        const int seconds = msec / 1000;
        return seconds == 1 ? QStringLiteral("1 second")
                            : QStringLiteral("%1 seconds").arg(seconds);
    }
    return QStringLiteral("%1 seconds").arg(msec / 1000.0, 0, 'f', 1);
}
