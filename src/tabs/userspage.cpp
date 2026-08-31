#include "userspage.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QHeaderView>
#include <QLabel>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

#include "../formatting.h"
#include "../sampler.h"
#include "theming.h"

namespace {

QString describeState(QChar state)
{
    switch (state.toLatin1()) {
    case 'R':
        return QCoreApplication::translate("ProcessModel", "Running");
    case 'S':
        return QCoreApplication::translate("ProcessModel", "Sleeping");
    case 'D':
        return QCoreApplication::translate("ProcessModel", "Disk wait");
    case 'Z':
        return QCoreApplication::translate("ProcessModel", "Zombie");
    case 'T':
        return QCoreApplication::translate("ProcessModel", "Stopped");
    case 't':
        return QCoreApplication::translate("ProcessModel", "Traced");
    case 'I':
        return QCoreApplication::translate("ProcessModel", "Idle");
    default:
        return QString(state);
    }
}

} // namespace

UsersModel::UsersModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

QModelIndex UsersModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColumnCount) {
        return {};
    }
    if (!parent.isValid()) {
        return row < m_users.size() ? createIndex(row, column, kUserLevel) : QModelIndex();
    }
    if (parent.internalId() != kUserLevel || parent.row() >= m_users.size()) {
        return {};
    }
    if (row >= m_users.at(parent.row()).processes.size()) {
        return {};
    }
    return createIndex(row, column, quintptr(parent.row() + 1));
}

QModelIndex UsersModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || child.internalId() == kUserLevel) {
        return {};
    }
    const int userRow = int(child.internalId()) - 1;
    if (userRow < 0 || userRow >= m_users.size()) {
        return {};
    }
    return createIndex(userRow, 0, kUserLevel);
}

int UsersModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        return m_users.size();
    }
    if (parent.internalId() != kUserLevel || parent.column() != 0) {
        return 0;
    }
    return parent.row() < m_users.size() ? m_users.at(parent.row()).processes.size() : 0;
}

int UsersModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return ColumnCount;
}

QString UsersModel::userNameAt(int row) const
{
    return row >= 0 && row < m_users.size() ? m_users.at(row).name : QString();
}

QVariant UsersModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal) {
        return {};
    }
    if (role == Qt::TextAlignmentRole && section != Name && section != Status) {
        return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    switch (static_cast<Column>(section)) {
    case Name:
        return tr("User / process");
    case Pid:
        return tr("PID");
    case Status:
        return tr("Status");
    case Cpu:
        return tr("CPU");
    case Memory:
        return tr("Memory");
    case ColumnCount:
        break;
    }
    return {};
}

QVariant UsersModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return {};
    }
    const auto column = static_cast<Column>(index.column());
    const bool isUserRow = index.internalId() == kUserLevel;

    if (role == Qt::TextAlignmentRole) {
        return column == Name || column == Status
            ? QVariant(Qt::AlignLeft | Qt::AlignVCenter)
            : QVariant(Qt::AlignRight | Qt::AlignVCenter);
    }

    if (isUserRow) {
        if (index.row() >= m_users.size()) {
            return {};
        }
        const UserGroup &user = m_users.at(index.row());

        if (role == Qt::ToolTipRole) {
            return tr("%1 (uid %2) · %3 processes")
                .arg(user.name)
                .arg(user.uid)
                .arg(user.processes.size());
        }
        if (role != Qt::DisplayRole && role != SortRole) {
            return {};
        }
        switch (column) {
        case Name:
            return role == SortRole
                ? QVariant(user.name.toLower())
                : QVariant(QStringLiteral("%1  (%2)").arg(user.name)
                               .arg(user.processes.size()));
        case Pid:
            return role == SortRole ? QVariant(0) : QVariant(QString());
        case Status:
            return user.sessionSummary;
        case Cpu:
            return role == SortRole
                ? QVariant(user.cpuPercent)
                : QVariant(QStringLiteral("%1%").arg(user.cpuPercent, 0, 'f', 1));
        case Memory:
            return role == SortRole ? QVariant(user.rssBytes)
                                    : QVariant(formatting::bytes(user.rssBytes));
        case ColumnCount:
            break;
        }
        return {};
    }

    const int userRow = int(index.internalId()) - 1;
    if (userRow < 0 || userRow >= m_users.size()) {
        return {};
    }
    const QVector<ProcessInfo> &processes = m_users.at(userRow).processes;
    if (index.row() >= processes.size()) {
        return {};
    }
    const ProcessInfo &process = processes.at(index.row());

    if (role == PidRole) {
        return process.pid;
    }
    if (role == Qt::ToolTipRole) {
        return process.command.isEmpty() ? process.name : process.command;
    }
    if (role != Qt::DisplayRole && role != SortRole) {
        return {};
    }
    switch (column) {
    case Name:
        return role == SortRole ? QVariant(process.name.toLower())
                                : QVariant(process.name);
    case Pid:
        return process.pid;
    case Status:
        return describeState(process.state);
    case Cpu:
        return role == SortRole
            ? QVariant(process.cpuPercent)
            : QVariant(QStringLiteral("%1%").arg(process.cpuPercent, 0, 'f', 1));
    case Memory:
        return role == SortRole ? QVariant(process.rssBytes)
                                : QVariant(formatting::bytes(process.rssBytes));
    case ColumnCount:
        break;
    }
    return {};
}

void UsersModel::syncProcesses(int userRow, QVector<ProcessInfo> &incoming)
{
    const QModelIndex parentIndex = createIndex(userRow, 0, kUserLevel);
    QVector<ProcessInfo> &current = m_users[userRow].processes;

    // Both lists are pid-sorted, so this is the same remove-then-insert walk
    // ProcessModel uses, restricted to one user's children.
    QSet<int> livePids;
    livePids.reserve(incoming.size());
    for (const ProcessInfo &process : incoming) {
        livePids.insert(process.pid);
    }

    for (int row = current.size() - 1; row >= 0; --row) {
        if (livePids.contains(current.at(row).pid)) {
            continue;
        }
        const int last = row;
        while (row > 0 && !livePids.contains(current.at(row - 1).pid)) {
            --row;
        }
        beginRemoveRows(parentIndex, row, last);
        current.remove(row, last - row + 1);
        endRemoveRows();
    }

    int row = 0;
    for (int j = 0; j < incoming.size(); ++j) {
        if (row < current.size() && current.at(row).pid == incoming.at(j).pid) {
            current[row] = incoming.at(j);
            ++row;
            continue;
        }
        const int runStart = j;
        while (j < incoming.size()
               && (row >= current.size() || current.at(row).pid != incoming.at(j).pid)) {
            ++j;
        }
        const int count = j - runStart;
        beginInsertRows(parentIndex, row, row + count - 1);
        for (int k = 0; k < count; ++k) {
            current.insert(row + k, incoming.at(runStart + k));
        }
        endInsertRows();
        row += count;
        --j;
    }
}

void UsersModel::update(const ProcessSnapshot &snapshot,
                        const QHash<uint, QVector<UserSession>> &sessions)
{
    // Group the flat snapshot by owning uid. Kernel threads are all root's
    // and would swamp the tree, so they are left out entirely.
    QHash<uint, QVector<ProcessInfo>> grouped;
    QHash<uint, QString> names;
    for (const ProcessInfo &process : snapshot.processes) {
        if (process.isKernelThread) {
            continue;
        }
        grouped[process.uid].append(process);
        names.insert(process.uid, process.user);
    }

    QVector<uint> uids = grouped.keys().toVector();
    std::sort(uids.begin(), uids.end(), [&names](uint a, uint b) {
        return names.value(a).compare(names.value(b), Qt::CaseInsensitive) < 0;
    });

    // Users come and go far more rarely than processes, so the top level is
    // reset only when the set actually changes.
    QVector<uint> currentUids;
    currentUids.reserve(m_users.size());
    for (const UserGroup &user : m_users) {
        currentUids.append(user.uid);
    }

    if (currentUids != uids) {
        beginResetModel();
        QVector<UserGroup> rebuilt;
        rebuilt.reserve(uids.size());
        for (const uint uid : uids) {
            UserGroup group;
            group.uid = uid;
            group.name = names.value(uid);
            // Carry the existing children over so the diff below sees them.
            for (const UserGroup &existing : m_users) {
                if (existing.uid == uid) {
                    group.processes = existing.processes;
                    break;
                }
            }
            rebuilt.append(group);
        }
        m_users = std::move(rebuilt);
        endResetModel();
    }

    for (int row = 0; row < m_users.size(); ++row) {
        UserGroup &user = m_users[row];
        QVector<ProcessInfo> incoming = grouped.value(user.uid);
        std::sort(incoming.begin(), incoming.end(),
                  [](const ProcessInfo &a, const ProcessInfo &b) { return a.pid < b.pid; });

        double cpu = 0.0;
        quint64 rss = 0;
        for (const ProcessInfo &process : incoming) {
            cpu += process.cpuPercent;
            rss += process.rssBytes;
        }
        user.cpuPercent = cpu;
        user.rssBytes = rss;
        user.sessionSummary = SessionProvider::describe(sessions.value(user.uid));

        syncProcesses(row, incoming);

        const QModelIndex userIndex = createIndex(row, 0, kUserLevel);
        emit dataChanged(userIndex, createIndex(row, ColumnCount - 1, kUserLevel));
        if (!user.processes.isEmpty()) {
            emit dataChanged(index(0, 0, userIndex),
                             index(user.processes.size() - 1, ColumnCount - 1, userIndex));
        }
    }
}

UsersPage::UsersPage(Sampler *sampler, QWidget *parent)
    : QWidget(parent)
    , m_model(new UsersModel(this))
    , m_view(new QTreeView(this))
    , m_summary(new QLabel(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    m_view->setModel(m_model);
    m_view->setUniformRowHeights(true);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_view->header()->setStretchLastSection(false);
    // Status carries the session summary, which is the longest and least
    // predictable value here, so it takes the slack instead of leaving a dead
    // strip to the right of the numbers.
    m_view->header()->setSectionResizeMode(UsersModel::Status, QHeaderView::Stretch);
    m_view->setColumnWidth(UsersModel::Name, 260);
    m_view->setColumnWidth(UsersModel::Pid, 80);
    m_view->setColumnWidth(UsersModel::Cpu, 90);
    m_view->setColumnWidth(UsersModel::Memory, 110);
    layout->addWidget(m_view, 1);

    layout->addWidget(m_summary);
    applyTheme();

    connect(sampler, &Sampler::processesSampled, this, &UsersPage::onProcessesSampled);
}

void UsersPage::applyTheme()
{
    theming::markSecondary(m_summary, palette());
}

void UsersPage::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

void UsersPage::onProcessesSampled(const ProcessSnapshot &snapshot)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_sessionsFetchedMsec == 0 || now - m_sessionsFetchedMsec > kSessionRefreshMsec) {
        m_cachedSessions = m_sessions.sessionsByUid();
        m_sessionsFetchedMsec = now;
    }

    // Rows stay collapsed: the tab's own subject is the per-user summary, and
    // expanding every account buries ten of them under one login's hundred
    // processes. Whatever the reader opens then stays open, which is what the
    // diffing in UsersModel::syncProcesses is for.
    m_model->update(snapshot, m_cachedSessions);

    int withSessions = 0;
    for (auto it = m_cachedSessions.cbegin(); it != m_cachedSessions.cend(); ++it) {
        if (!it.value().isEmpty()) {
            ++withSessions;
        }
    }

    QString text = tr("%n user(s) with running processes", "",
                      m_model->userCount());
    if (m_sessions.isAvailable() && withSessions > 0) {
        text += QStringLiteral("  ·  ") + tr("%1 logged in").arg(withSessions);
    } else if (!m_sessions.isAvailable()) {
        text += QStringLiteral("  ·  ")
            + tr("login sessions unavailable (systemd-logind not reachable)");
    }
    m_summary->setText(text);
}
