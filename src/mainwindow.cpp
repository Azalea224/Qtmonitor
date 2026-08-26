#include "mainwindow.h"

#include <QCloseEvent>
#include <QIcon>
#include <QTabWidget>
#include <QToolButton>

#include <KConfigGroup>
#include <KSharedConfig>

#include "sampler.h"
#include "settings.h"
#include "settingsdialog.h"
#include "tabs/performancepage.h"
#include "tabs/placeholderpage.h"
#include "tabs/processespage.h"
#include "tabs/startuppage.h"
#include "tabs/userspage.h"

namespace {
// Scanning every /proc/PID is only worth doing while a tab that consumes the
// process list is on screen. Two of them do.
constexpr int kProcessesTab = 0;
constexpr int kPerformanceTab = 1;
constexpr int kUsersTab = 3;

bool needsProcessSampling(int tabIndex)
{
    return tabIndex == kProcessesTab || tabIndex == kUsersTab;
}

// Disk and network counters are cheap, but the disk provider also stats every
// mounted filesystem, so it follows the same visible-tab rule. Only the
// Performance tab draws them.
bool needsResourceSampling(int tabIndex)
{
    return tabIndex == kPerformanceTab;
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_tabs(new QTabWidget(this))
    , m_sampler(new Sampler(this))
{
    setWindowTitle(QStringLiteral("Qtmonitor"));

    m_tabs->addTab(new ProcessesPage(m_sampler, this), QStringLiteral("Processes"));
    m_tabs->addTab(new PerformancePage(m_sampler, this), QStringLiteral("Performance"));
    m_tabs->addTab(new StartupPage(this), QStringLiteral("Startup Apps"));
    m_tabs->addTab(new UsersPage(m_sampler, this), QStringLiteral("Users"));
    m_tabs->addTab(new PlaceholderPage(4, this), QStringLiteral("Details"));

    // A corner button rather than a menu bar: one action does not justify a
    // menu, and it keeps the window chrome identical on every desktop.
    auto *settingsButton = new QToolButton(this);
    settingsButton->setText(QStringLiteral("Settings"));
    settingsButton->setToolTip(QStringLiteral("Update speed, units and appearance"));
    settingsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    settingsButton->setAutoRaise(true);
    settingsButton->setIcon(QIcon::fromTheme(
        QStringLiteral("configure"),
        QIcon::fromTheme(QStringLiteral("preferences-system"),
                         QIcon(QStringLiteral(":/icons/configure.svg")))));
    connect(settingsButton, &QToolButton::clicked, this, &MainWindow::openSettings);
    m_tabs->setCornerWidget(settingsButton, Qt::TopRightCorner);

    setCentralWidget(m_tabs);

    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        m_sampler->setProcessSamplingEnabled(needsProcessSampling(index));
        m_sampler->setResourceSamplingEnabled(needsResourceSampling(index));
    });
    m_sampler->setProcessSamplingEnabled(needsProcessSampling(m_tabs->currentIndex()));
    m_sampler->setResourceSamplingEnabled(needsResourceSampling(m_tabs->currentIndex()));

    m_sampler->setInterval(Settings::instance().pollIntervalMsec());
    connect(&Settings::instance(), &Settings::pollIntervalChanged, m_sampler,
            &Sampler::setInterval);

    m_sampler->start();

    restoreSettings();
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::openSettings()
{
    if (!m_settingsDialog) {
        m_settingsDialog = new SettingsDialog(this);
    }
    m_settingsDialog->show();
    m_settingsDialog->raise();
    m_settingsDialog->activateWindow();
}

void MainWindow::restoreSettings()
{
    const KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("MainWindow"));
    const QByteArray geometry = group.readEntry("geometry", QByteArray());
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    } else {
        resize(1024, 720);
    }
}

void MainWindow::saveSettings()
{
    KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("MainWindow"));
    group.writeEntry("geometry", saveGeometry());
    group.sync();
}
