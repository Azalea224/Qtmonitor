#include "settingsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "settings.h"
#include "tabs/theming.h"

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
    , m_interval(new QComboBox(this))
    , m_temperature(new QComboBox(this))
    , m_theme(new QComboBox(this))
    , m_kernelThreads(new QCheckBox(tr("Show kernel threads in the process list"),
                                    this))
    , m_note(new QLabel(tr("Graphs keep a 60-second window at every update "
                           "speed."),
                        this))
{
    setWindowTitle(tr("Qtmonitor Settings"));

    Settings &settings = Settings::instance();

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addLayout(form);

    const QVector<int> intervals = Settings::pollIntervalChoices();
    for (const int msec : intervals) {
        m_interval->addItem(Settings::describePollInterval(msec), msec);
    }
    // A value restored from a hand-edited config need not be one of the
    // offered speeds, so it is added rather than silently snapped to one.
    if (m_interval->findData(settings.pollIntervalMsec()) < 0) {
        m_interval->addItem(Settings::describePollInterval(settings.pollIntervalMsec()),
                            settings.pollIntervalMsec());
    }
    m_interval->setCurrentIndex(m_interval->findData(settings.pollIntervalMsec()));
    form->addRow(tr("Update speed:"), m_interval);

    m_temperature->addItem(tr("Celsius (°C)"),
                           int(Settings::TemperatureUnit::Celsius));
    m_temperature->addItem(tr("Fahrenheit (°F)"),
                           int(Settings::TemperatureUnit::Fahrenheit));
    m_temperature->setCurrentIndex(m_temperature->findData(int(settings.temperatureUnit())));
    form->addRow(tr("Temperature:"), m_temperature);

    m_theme->addItem(tr("Follow the desktop"), int(Settings::Theme::System));
    m_theme->addItem(tr("Light"), int(Settings::Theme::Light));
    m_theme->addItem(tr("Dark"), int(Settings::Theme::Dark));
    m_theme->setCurrentIndex(m_theme->findData(int(settings.theme())));
    form->addRow(tr("Appearance:"), m_theme);

    m_kernelThreads->setChecked(settings.showKernelThreads());
    form->addRow(QString(), m_kernelThreads);

    theming::markSecondary(m_note, palette());
    layout->addWidget(m_note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    connect(m_interval, &QComboBox::currentIndexChanged, this, [this, &settings] {
        settings.setPollIntervalMsec(m_interval->currentData().toInt());
    });
    connect(m_temperature, &QComboBox::currentIndexChanged, this, [this, &settings] {
        settings.setTemperatureUnit(
            static_cast<Settings::TemperatureUnit>(m_temperature->currentData().toInt()));
    });
    connect(m_theme, &QComboBox::currentIndexChanged, this, [this, &settings] {
        settings.setTheme(static_cast<Settings::Theme>(m_theme->currentData().toInt()));
    });
    connect(m_kernelThreads, &QCheckBox::toggled, &settings,
            &Settings::setShowKernelThreads);

    // The Processes tab has its own checkbox for the same setting, so follow
    // it while this dialog is open.
    connect(&settings, &Settings::showKernelThreadsChanged, m_kernelThreads,
            &QCheckBox::setChecked);
}

void SettingsDialog::changeEvent(QEvent *event)
{
    // Switching the appearance from this very dialog repaints it, so the
    // dimmed note has to be re-derived from the new palette.
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        theming::markSecondary(m_note, palette());
    }
    QDialog::changeEvent(event);
}
