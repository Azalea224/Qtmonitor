#include "mainwindow.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QListWidget>
#include <QStackedWidget>

#include <KConfigGroup>
#include <KSharedConfig>

#include "tabs/placeholderpage.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_nav(new QListWidget(this))
    , m_pages(new QStackedWidget(this))
{
    setWindowTitle(QStringLiteral("Qtmonitor"));

    addPage(QStringLiteral("Processes"), QStringLiteral("system-run"), new PlaceholderPage(3, this));
    addPage(QStringLiteral("Performance"), QStringLiteral("view-statistics"), new PlaceholderPage(2, this));
    addPage(QStringLiteral("Startup Apps"), QStringLiteral("applications-other"), new PlaceholderPage(5, this));
    addPage(QStringLiteral("Users"), QStringLiteral("system-users"), new PlaceholderPage(5, this));
    addPage(QStringLiteral("Details"), QStringLiteral("view-list-details"), new PlaceholderPage(4, this));

    m_nav->setIconSize(QSize(22, 22));
    m_nav->setSpacing(2);
    m_nav->setUniformItemSizes(true);
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->setMinimumWidth(170);
    m_nav->setMaximumWidth(220);
    m_nav->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    connect(m_nav, &QListWidget::currentRowChanged,
            m_pages, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_nav);
    layout->addWidget(m_pages, 1);
    setCentralWidget(central);

    restoreSettings();
}

MainWindow::~MainWindow() = default;

void MainWindow::addPage(const QString &title, const QString &iconName, QWidget *page)
{
    const QIcon fallback(QStringLiteral(":/icons/") + iconName + QStringLiteral(".svg"));
    auto *item = new QListWidgetItem(QIcon::fromTheme(iconName, fallback), title, m_nav);
    item->setSizeHint(QSize(item->sizeHint().width(), 40));
    m_pages->addWidget(page);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    QMainWindow::closeEvent(event);
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
