#include "startuppage.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QTreeView>
#include <QVBoxLayout>

#include "theming.h"

namespace {

QString describeKind(StartupEntry::Kind kind)
{
    switch (kind) {
    case StartupEntry::Kind::XdgAutostart:
        return QStringLiteral("Autostart");
    case StartupEntry::Kind::SystemdUser:
        return QStringLiteral("User service");
    }
    return {};
}

QString describeStatus(const StartupEntry &entry)
{
    switch (entry.status) {
    case StartupEntry::Status::Enabled:
        return QStringLiteral("Enabled");
    case StartupEntry::Status::Disabled:
        return QStringLiteral("Disabled");
    case StartupEntry::Status::OtherDesktop:
        return QStringLiteral("Other desktop");
    case StartupEntry::Status::Missing:
        return QStringLiteral("Program missing");
    }
    return {};
}

} // namespace

StartupModel::StartupModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int StartupModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int StartupModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

const StartupEntry *StartupModel::entryAt(int row) const
{
    if (row < 0 || row >= m_rows.size()) {
        return nullptr;
    }
    return &m_rows.at(row);
}

void StartupModel::setEntries(const QVector<StartupEntry> &entries)
{
    beginResetModel();
    m_rows = entries;
    endResetModel();
}

QVariant StartupModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (static_cast<Column>(section)) {
    case Name:
        return QStringLiteral("Name");
    case Kind:
        return QStringLiteral("Type");
    case Status:
        return QStringLiteral("Status");
    case Detail:
        return QStringLiteral("Details");
    case ColumnCount:
        break;
    }
    return {};
}

QVariant StartupModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return {};
    }

    const StartupEntry &entry = m_rows.at(index.row());
    const auto column = static_cast<Column>(index.column());

    if (role == EnabledRole) {
        return entry.isEnabled();
    }

    if (role == Qt::ToolTipRole) {
        QString tip = entry.sourcePath.isEmpty() ? entry.id : entry.sourcePath;
        if (!entry.command.isEmpty()) {
            tip += QStringLiteral("\n%1").arg(entry.command);
        }
        if (entry.status == StartupEntry::Status::OtherDesktop) {
            tip += QStringLiteral("\nRuns only on: %1").arg(entry.desktopRestriction);
        }
        return tip;
    }

    if (role != Qt::DisplayRole && role != SortRole) {
        return {};
    }

    switch (column) {
    case Name:
        return entry.name;
    case Kind:
        return describeKind(entry.kind);
    case Status:
        return describeStatus(entry);
    case Detail:
        if (entry.status == StartupEntry::Status::OtherDesktop) {
            return QStringLiteral("Only on %1").arg(entry.desktopRestriction);
        }
        if (!entry.description.isEmpty()) {
            return entry.description;
        }
        return entry.command;
    case ColumnCount:
        break;
    }
    return {};
}

StartupPage::StartupPage(QWidget *parent)
    : QWidget(parent)
    , m_model(new StartupModel(this))
    , m_proxy(new QSortFilterProxyModel(this))
    , m_view(new QTreeView(this))
    , m_search(new QLineEdit(this))
    , m_toggleButton(new QPushButton(QStringLiteral("Enable"), this))
    , m_summary(new QLabel(this))
{
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(StartupModel::SortRole);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1); // match against every column

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *toolbar = new QHBoxLayout;
    m_search->setPlaceholderText(QStringLiteral("Search startup items"));
    m_search->setClearButtonEnabled(true);
    toolbar->addWidget(m_search, 1);
    toolbar->addWidget(m_toggleButton);
    layout->addLayout(toolbar);

    m_view->setModel(m_proxy);
    m_view->setRootIsDecorated(false);
    m_view->setUniformRowHeights(true);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(StartupModel::Name, Qt::AscendingOrder);
    m_view->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_view->header()->setStretchLastSection(true);
    for (int column = 0; column < StartupModel::ColumnCount; ++column) {
        m_view->setColumnWidth(column, column == StartupModel::Name ? 280 : 130);
    }
    layout->addWidget(m_view, 1);

    layout->addWidget(m_summary);
    applyTheme();

    m_toggleButton->setEnabled(false);

    connect(m_search, &QLineEdit::textChanged, m_proxy,
            &QSortFilterProxyModel::setFilterFixedString);
    connect(m_toggleButton, &QPushButton::clicked, this, &StartupPage::toggleSelected);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateButton(); });
    connect(m_view, &QAbstractItemView::doubleClicked, this,
            [this] { toggleSelected(); });
}

void StartupPage::applyTheme()
{
    theming::markSecondary(m_summary, palette());
}

void StartupPage::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

void StartupPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Scanning spawns systemctl, so the first scan waits until the tab is
    // opened rather than running during startup. Later opens re-scan as well:
    // these entries are files anything can edit, and ~30ms on a tab switch
    // beats showing a list that quietly went stale.
    refresh();
}

const StartupEntry *StartupPage::selectedEntry() const
{
    const QModelIndex index = m_view->currentIndex();
    if (!index.isValid()) {
        return nullptr;
    }
    return m_model->entryAt(m_proxy->mapToSource(index).row());
}

void StartupPage::updateButton()
{
    const StartupEntry *entry = selectedEntry();
    if (!entry || !entry->toggleable()) {
        m_toggleButton->setEnabled(false);
        m_toggleButton->setText(QStringLiteral("Enable"));
        return;
    }
    m_toggleButton->setEnabled(true);
    m_toggleButton->setText(entry->isEnabled() ? QStringLiteral("Disable")
                                               : QStringLiteral("Enable"));
}

void StartupPage::refresh()
{
    // Remember what was selected. The scan rebuilds every row, and losing the
    // selection leaves the toggle button inert immediately after a toggle,
    // which is precisely when it is about to be pressed again.
    QString selectedId;
    StartupEntry::Kind selectedKind = StartupEntry::Kind::XdgAutostart;
    if (const StartupEntry *entry = selectedEntry()) {
        selectedId = entry->id;
        selectedKind = entry->kind;
    }

    const QVector<StartupEntry> entries = m_provider.scan();
    m_model->setEntries(entries);

    int enabled = 0;
    int otherDesktop = 0;
    for (const StartupEntry &entry : entries) {
        if (entry.isEnabled()) {
            ++enabled;
        } else if (entry.status == StartupEntry::Status::OtherDesktop) {
            ++otherDesktop;
        }
    }

    QString text = QStringLiteral("%1 items  ·  %2 enabled")
                       .arg(entries.size())
                       .arg(enabled);
    if (otherDesktop > 0) {
        // Worth calling out on a non-Plasma, non-GNOME desktop, where a good
        // number of packaged autostart entries simply never fire.
        const QStringList desktops = StartupProvider::currentDesktops();
        text += QStringLiteral("  ·  %1 for other desktops (this is %2)")
                    .arg(otherDesktop)
                    .arg(desktops.isEmpty() ? QStringLiteral("unset")
                                            : desktops.join(QLatin1Char('/')));
    }
    m_summary->setText(text);

    for (int row = 0; row < entries.size() && !selectedId.isEmpty(); ++row) {
        if (entries.at(row).id != selectedId || entries.at(row).kind != selectedKind) {
            continue;
        }
        const QModelIndex index = m_proxy->mapFromSource(m_model->index(row, 0));
        if (index.isValid()) {
            m_view->setCurrentIndex(index);
        }
        break;
    }

    updateButton();
}

void StartupPage::toggleSelected()
{
    const StartupEntry *entry = selectedEntry();
    if (!entry || !entry->toggleable()) {
        return;
    }

    // Copied before the scan below invalidates the pointer.
    const StartupEntry target = *entry;
    const bool enable = !target.isEnabled();

    const QString error = m_provider.setEnabled(target, enable);
    if (!error.isEmpty()) {
        QMessageBox::warning(
            this,
            enable ? QStringLiteral("Could not enable") : QStringLiteral("Could not disable"),
            QStringLiteral("%1 could not be %2.\n\n%3")
                .arg(target.name,
                     enable ? QStringLiteral("enabled") : QStringLiteral("disabled"),
                     error));
    }

    // Re-scan either way: on success it shows the new state, and on failure
    // it proves the entry really is unchanged.
    refresh();
}
