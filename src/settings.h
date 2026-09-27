#pragma once

#include <QObject>
#include <QPalette>
#include <QString>
#include <QVector>

// Every user-facing preference, stored in KConfig's "General" group.
//
// A singleton, because these values are read all over the widget tree — a
// graph needs the poll interval to label its own X axis honestly — and
// threading a settings pointer through every constructor would buy nothing in
// a single-window app. Each setter writes through to disk immediately and
// signals, so the UI applies changes live rather than on an OK button.
class Settings : public QObject
{
    Q_OBJECT

public:
    enum class TemperatureUnit {
        Celsius,
        Fahrenheit,
    };

    enum class Theme {
        System, // whatever the platform theme reports
        Light,
        Dark,
    };

    static Settings &instance();

    int pollIntervalMsec() const { return m_pollIntervalMsec; }
    void setPollIntervalMsec(int msec);

    TemperatureUnit temperatureUnit() const { return m_temperatureUnit; }
    void setTemperatureUnit(TemperatureUnit unit);

    Theme theme() const { return m_theme; }
    void setTheme(Theme theme);

    bool showKernelThreads() const { return m_showKernelThreads; }
    void setShowKernelThreads(bool show);

    // Pushes the theme choice into Qt: the color-scheme style hint plus an
    // explicit palette.
    //
    // The hint alone is not enough. A platform theme that supplies its own
    // palette (qtengine, qt6ct, KDE's) keeps it whatever the hint says, and
    // Fusion's standardPalette() ignores the hint too — verified on Qt 6.11.
    // An explicit QPalette is the only override that works on every desktop,
    // which is also what the project's portability rules call for.
    void applyTheme() const;

    // Sensors are read in °C; this renders in whichever unit is configured.
    QString formatTemperature(double celsius) const;

    // The same conversion without the formatting, for callers that need the
    // number itself — a chart plots in the displayed unit, and having it do
    // its own °F arithmetic would put the conversion in two places.
    double toDisplayTemperature(double celsius) const;

    // How many samples cover `seconds` of wall clock at the current update
    // speed. The graphs keep a fixed 60-second window whatever the speed, so
    // their "60s ago" axis label stays true instead of silently meaning
    // "60 samples ago".
    int sampleCount(int seconds) const;

    // Update speeds offered by the settings dialog, in milliseconds.
    static QVector<int> pollIntervalChoices();
    static QString describePollInterval(int msec);

signals:
    void pollIntervalChanged(int msec);
    void temperatureUnitChanged();
    void showKernelThreadsChanged(bool show);

private:
    Settings();

    // The desktop's own palette, captured before any override is applied, so
    // "follow the desktop" has something to go back to. Qt stops propagating
    // platform palette changes to an application that has set its own, so a
    // desktop-wide light/dark switch after an override is only picked up on
    // the next start.
    QPalette m_systemPalette;

    int m_pollIntervalMsec = 1000;
    TemperatureUnit m_temperatureUnit = TemperatureUnit::Celsius;
    Theme m_theme = Theme::System;
    bool m_showKernelThreads = false;
};
