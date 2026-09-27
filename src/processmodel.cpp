#include "processmodel.h"

#include <QCoreApplication>
#include <QSet>

#include <algorithm>
#include <map>

#include "formatting.h"

namespace {

QString formatRate(quint64 bytesPerSec, bool accessible)
{
    if (!accessible) {
        // Distinct from "0 B/s": we are not allowed to read this counter.
        return QStringLiteral("—");
    }
    if (bytesPerSec == 0) {
        return QStringLiteral("0 B/s");
    }
    return formatting::bytes(bytesPerSec) + QStringLiteral("/s");
}

// A negative share means no GPU backend could measure this process — it
// belongs to another user, or the only card present is driven by something
// that reports nothing per-process. Distinct from a measured 0%.
QString formatGpuPercent(double percent)
{
    if (percent < 0.0) {
        return QStringLiteral("—");
    }
    return QStringLiteral("%1%").arg(percent, 0, 'f', 1);
}

// Linux process states, spelled out rather than left as single letters.
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
    case 'X':
        return QCoreApplication::translate("ProcessModel", "Dead");
    default:
        return QString(state);
    }
}

} // namespace

ProcessModel::ProcessModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

ProcessModel::~ProcessModel() = default;

// A top-level index carries no pointer; a child index points at its group.
ProcessModel::Group *ProcessModel::groupOf(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return nullptr;
    }
    if (auto *group = static_cast<Group *>(index.internalPointer())) {
        return group;
    }
    return index.row() < int(m_groups.size()) ? m_groups[index.row()].get()
                                              : nullptr;
}

QModelIndex ProcessModel::index(int row, int column,
                                const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColumnCount) {
        return {};
    }
    if (!parent.isValid()) {
        return row < int(m_groups.size()) ? createIndex(row, column, nullptr)
                                           : QModelIndex();
    }
    if (parent.internalPointer() || parent.row() >= int(m_groups.size())) {
        return {}; // children have no children of their own
    }
    Group *group = m_groups[parent.row()].get();
    if (!group->hasChildren || row >= group->members.size()) {
        return {};
    }
    return createIndex(row, column, group);
}

QModelIndex ProcessModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || !child.internalPointer()) {
        return {};
    }
    const auto *group = static_cast<const Group *>(child.internalPointer());
    return createIndex(group->row, 0, nullptr);
}

int ProcessModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        return int(m_groups.size());
    }
    if (parent.internalPointer() || parent.column() != 0
        || parent.row() >= int(m_groups.size())) {
        return 0;
    }
    const Group &group = *m_groups[parent.row()];
    return group.hasChildren ? group.members.size() : 0;
}

int ProcessModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return ColumnCount;
}

const ProcessInfo *ProcessModel::processAt(const QModelIndex &index) const
{
    const Group *group = groupOf(index);
    if (!group) {
        return nullptr;
    }
    if (index.internalPointer()) {
        return index.row() < group->members.size() ? &group->members.at(index.row())
                                                   : nullptr;
    }
    return group->hasChildren || group->members.isEmpty() ? nullptr
                                                          : &group->members.first();
}

QString ProcessModel::columnTitle(Column column)
{
    switch (column) {
    case Name:
        return tr("Name");
    case Pid:
        return tr("PID");
    case User:
        return tr("User");
    case Cpu:
        return tr("CPU");
    case Memory:
        return tr("Memory");
    case State:
        return tr("State");
    case Nice:
        return tr("Nice");
    case Threads:
        return tr("Threads");
    case DiskRead:
        return tr("Disk read");
    case DiskWrite:
        return tr("Disk write");
    case GpuCompute:
        return tr("GPU");
    case GpuVideo:
        return tr("GPU video");
    case Command:
        return tr("Command line");
    case ColumnCount:
        break;
    }
    return {};
}

bool ProcessModel::columnHiddenByDefault(Column column)
{
    switch (column) {
    case Nice:
    case DiskRead:
    case DiskWrite:
    case GpuCompute:
    case GpuVideo:
    case Command:
        return true;
    default:
        return false;
    }
}

QVariant ProcessModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const
{
    if (orientation != Qt::Horizontal || section < 0 || section >= ColumnCount) {
        return {};
    }
    if (role == Qt::DisplayRole) {
        return columnTitle(static_cast<Column>(section));
    }
    if (role == Qt::TextAlignmentRole && section != Name && section != User
        && section != State && section != Command) {
        return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    }
    return {};
}

QVariant ProcessModel::data(const QModelIndex &index, int role) const
{
    const Group *group = groupOf(index);
    if (!group) {
        return {};
    }
    const auto column = static_cast<Column>(index.column());

    if (role == Qt::TextAlignmentRole) {
        switch (column) {
        case Name:
        case User:
        case State:
        case Command:
            return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        default:
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        }
    }

    if (const ProcessInfo *p = processAt(index)) {
        return processData(*p, column, role);
    }
    return groupData(*group, column, role);
}

QVariant ProcessModel::processData(const ProcessInfo &p, Column column,
                                   int role) const
{
    if (role == PidRole) {
        return p.pid;
    }
    if (role == KernelThreadRole) {
        return p.isKernelThread;
    }
    if (role == StartTicksRole) {
        return p.startTicks;
    }

    if (role == Qt::ToolTipRole) {
        // Kernel threads have no cmdline, so fall back to something useful.
        return p.command.isEmpty()
            ? tr("%1 (kernel thread, PID %2)").arg(p.name).arg(p.pid)
            : p.command;
    }

    // Raw values for sorting; the proxy compares these instead of the text.
    if (role == SortRole) {
        switch (column) {
        case Name:
            return p.name.toLower();
        case Pid:
            return p.pid;
        case User:
            return p.user;
        case Cpu:
            return p.cpuPercent;
        case Memory:
            return p.rssBytes;
        case State:
            return describeState(p.state);
        case Nice:
            return p.nice;
        case Threads:
            return p.threads;
        case DiskRead:
            return p.diskReadBytesPerSec;
        case DiskWrite:
            return p.diskWriteBytesPerSec;
        case GpuCompute:
            return p.gpuComputePercent;
        case GpuVideo:
            return p.gpuVideoPercent;
        case Command:
            return p.command;
        case ColumnCount:
            break;
        }
        return {};
    }

    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (column) {
    case Name:
        return p.name;
    case Pid:
        return p.pid;
    case User:
        return p.user;
    case Cpu:
        return QStringLiteral("%1%").arg(p.cpuPercent, 0, 'f', 1);
    case Memory:
        return formatting::bytes(p.rssBytes);
    case State:
        return describeState(p.state);
    case Nice:
        return p.nice;
    case Threads:
        return p.threads;
    case DiskRead:
        return formatRate(p.diskReadBytesPerSec, p.ioAccessible);
    case DiskWrite:
        return formatRate(p.diskWriteBytesPerSec, p.ioAccessible);
    case GpuCompute:
        return formatGpuPercent(p.gpuComputePercent);
    case GpuVideo:
        return formatGpuPercent(p.gpuVideoPercent);
    case Command:
        return p.command;
    case ColumnCount:
        break;
    }
    return {};
}

QVariant ProcessModel::groupData(const Group &group, Column column, int role) const
{
    const QVector<ProcessInfo> &members = group.members;
    // Mid-diff, a group whose every pid was replaced is briefly empty, and
    // the recursive proxy re-filters it right then. Only its key is known.
    if (members.isEmpty()) {
        if (column == Name && (role == Qt::DisplayRole || role == SortRole)) {
            return role == SortRole ? group.key.first.toLower() : group.key.first;
        }
        return role == KernelThreadRole ? QVariant(group.key.second) : QVariant();
    }
    const ProcessInfo &first = members.first();

    // A group has no PID of its own: it cannot be signalled as one process,
    // so End process stays disabled until a child row is picked.
    if (role == PidRole || role == StartTicksRole) {
        return 0;
    }
    if (role == KernelThreadRole) {
        return group.key.second;
    }
    if (role == Qt::ToolTipRole) {
        return tr("%1: %n process(es)", "group tooltip", members.size())
            .arg(first.name);
    }
    if (role != Qt::DisplayRole && role != SortRole) {
        return {};
    }
    const bool sorting = role == SortRole;

    // A column shows a value when every member agrees on it, and is left
    // blank rather than guessed when they differ.
    const auto allSame = [&members](auto field) {
        return std::all_of(members.cbegin(), members.cend(),
                           [&](const ProcessInfo &p) { return field(p) == field(members.first()); });
    };

    switch (column) {
    case Name:
        return sorting ? QVariant(first.name.toLower())
                       : QVariant(QStringLiteral("%1  (%2)").arg(first.name)
                                      .arg(members.size()));
    case Pid:
        // Lowest member pid, so sorting by PID still orders groups sensibly.
        return sorting ? QVariant(first.pid) : QVariant(QString());
    case User:
        if (allSame([](const ProcessInfo &p) { return p.uid; })) {
            return first.user;
        }
        return sorting ? QVariant(QString())
                       : QVariant(tr("%n user(s)", "", [&members] {
                             QSet<uint> uids;
                             for (const ProcessInfo &p : members) {
                                 uids.insert(p.uid);
                             }
                             return int(uids.size());
                         }()));
    case Cpu:
        return sorting ? QVariant(group.cpuPercent)
                       : QVariant(QStringLiteral("%1%").arg(group.cpuPercent, 0, 'f', 1));
    case Memory:
        return sorting ? QVariant(group.rssBytes)
                       : QVariant(formatting::bytes(group.rssBytes));
    case State:
        return allSame([](const ProcessInfo &p) { return p.state; })
            ? describeState(first.state)
            : QString();
    case Nice:
        if (allSame([](const ProcessInfo &p) { return p.nice; })) {
            return first.nice;
        }
        return sorting ? QVariant(first.nice) : QVariant(QString());
    case Threads:
        return group.threads;
    case DiskRead:
        return sorting ? QVariant(group.diskReadBytesPerSec)
                       : QVariant(formatRate(group.diskReadBytesPerSec, group.ioAccessible));
    case DiskWrite:
        return sorting ? QVariant(group.diskWriteBytesPerSec)
                       : QVariant(formatRate(group.diskWriteBytesPerSec, group.ioAccessible));
    case GpuCompute:
        return sorting ? QVariant(group.gpuComputePercent)
                       : QVariant(formatGpuPercent(group.gpuComputePercent));
    case GpuVideo:
        return sorting ? QVariant(group.gpuVideoPercent)
                       : QVariant(formatGpuPercent(group.gpuVideoPercent));
    case Command:
        return QString();
    case ColumnCount:
        break;
    }
    return {};
}

void ProcessModel::computeTotals(Group &group)
{
    group.cpuPercent = 0.0;
    group.rssBytes = 0;
    group.threads = 0;
    group.diskReadBytesPerSec = 0;
    group.diskWriteBytesPerSec = 0;
    group.ioAccessible = false;
    group.gpuComputePercent = -1.0;
    group.gpuVideoPercent = -1.0;

    // An unmeasured member (negative GPU share, unreadable I/O) is left out
    // of the sum rather than counted as 0; the total is unknown only when no
    // member could be measured at all.
    const auto addMeasured = [](double &total, double value) {
        if (value >= 0.0) {
            total = total < 0.0 ? value : total + value;
        }
    };

    for (const ProcessInfo &p : group.members) {
        group.cpuPercent += p.cpuPercent;
        group.rssBytes += p.rssBytes;
        group.threads += p.threads;
        if (p.ioAccessible) {
            group.ioAccessible = true;
            group.diskReadBytesPerSec += p.diskReadBytesPerSec;
            group.diskWriteBytesPerSec += p.diskWriteBytesPerSec;
        }
        addMeasured(group.gpuComputePercent, p.gpuComputePercent);
        addMeasured(group.gpuVideoPercent, p.gpuVideoPercent);
    }
}

void ProcessModel::reindexGroups(int from)
{
    for (int row = from; row < int(m_groups.size()); ++row) {
        m_groups[row]->row = row;
    }
}

void ProcessModel::syncMembers(Group &group, QVector<ProcessInfo> &incoming)
{
    const QModelIndex groupIndex = createIndex(group.row, 0, nullptr);
    const bool wantChildren = incoming.size() > 1;

    // Becoming or ceasing to be a group swaps the whole child list at once.
    if (!wantChildren) {
        if (group.hasChildren) {
            beginRemoveRows(groupIndex, 0, group.members.size() - 1);
            group.members = std::move(incoming);
            group.hasChildren = false;
            endRemoveRows();
        } else {
            group.members = std::move(incoming);
        }
        return;
    }
    if (!group.hasChildren) {
        beginInsertRows(groupIndex, 0, incoming.size() - 1);
        group.members = std::move(incoming);
        group.hasChildren = true;
        endInsertRows();
        return;
    }

    // Both lists are sorted by pid. Remove the exited processes first, which
    // leaves the current list as a subsequence of the incoming one.
    QVector<ProcessInfo> &current = group.members;
    QSet<int> livePids;
    livePids.reserve(incoming.size());
    for (const ProcessInfo &p : incoming) {
        livePids.insert(p.pid);
    }

    for (int row = current.size() - 1; row >= 0; --row) {
        if (livePids.contains(current.at(row).pid)) {
            continue;
        }
        const int last = row;
        while (row > 0 && !livePids.contains(current.at(row - 1).pid)) {
            --row;
        }
        beginRemoveRows(groupIndex, row, last);
        current.remove(row, last - row + 1);
        endRemoveRows();
    }

    // Walk both lists together: matching pids are refreshed in place, and
    // runs of new pids are inserted at the position that keeps pid order.
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
        beginInsertRows(groupIndex, row, row + count - 1);
        for (int k = 0; k < count; ++k) {
            current.insert(row + k, incoming.at(runStart + k));
        }
        endInsertRows();
        row += count;
        --j; // the outer ++j re-examines the row that stopped the run
    }
}

void ProcessModel::setSnapshot(const ProcessSnapshot &snapshot)
{
    // Bucket the flat, pid-sorted snapshot by key. std::map keeps the keys
    // in the same order m_groups is held in, and each bucket stays pid-sorted
    // because the snapshot already is.
    std::map<GroupKey, QVector<ProcessInfo>> incoming;
    for (const ProcessInfo &p : snapshot.processes) {
        incoming[{p.name, p.isKernelThread}].append(p);
    }

    // Remove the groups whose name has gone, in contiguous runs.
    for (int row = int(m_groups.size()) - 1; row >= 0; --row) {
        if (incoming.count(m_groups[row]->key)) {
            continue;
        }
        const int last = row;
        while (row > 0 && !incoming.count(m_groups[row - 1]->key)) {
            --row;
        }
        beginRemoveRows(QModelIndex(), row, last);
        m_groups.erase(m_groups.begin() + row, m_groups.begin() + last + 1);
        reindexGroups(row);
        endRemoveRows();
    }

    // Walk both key-sorted lists together: surviving groups have their
    // members diffed in place, and runs of new names are inserted where they
    // keep the order.
    int row = 0;
    for (auto it = incoming.begin(); it != incoming.end(); ++it) {
        if (row < int(m_groups.size()) && m_groups[row]->key == it->first) {
            Group &group = *m_groups[row];
            syncMembers(group, it->second);
            computeTotals(group);
            ++row;
            continue;
        }
        std::vector<std::unique_ptr<Group>> run;
        while (it != incoming.end()
               && (row >= int(m_groups.size()) || m_groups[row]->key != it->first)) {
            auto group = std::make_unique<Group>();
            group->key = it->first;
            group->members = std::move(it->second);
            group->hasChildren = group->members.size() > 1;
            computeTotals(*group);
            run.push_back(std::move(group));
            ++it;
        }
        const int count = int(run.size());
        beginInsertRows(QModelIndex(), row, row + count - 1);
        m_groups.insert(m_groups.begin() + row, std::make_move_iterator(run.begin()),
                        std::make_move_iterator(run.end()));
        reindexGroups(row);
        endInsertRows();
        row += count;
        if (it == incoming.end()) {
            break;
        }
        --it; // the loop's ++it re-examines the key that stopped the run
    }

    if (m_groups.empty()) {
        return;
    }
    // Coarse notifications: the view only repaints visible rows, so this is
    // cheaper than tracking which cells actually moved. Children are only
    // announced for groups that have any.
    emit dataChanged(index(0, 0), index(int(m_groups.size()) - 1, ColumnCount - 1));
    for (const auto &group : m_groups) {
        if (group->hasChildren) {
            const QModelIndex parent = createIndex(group->row, 0, nullptr);
            emit dataChanged(index(0, 0, parent),
                             index(group->members.size() - 1, ColumnCount - 1, parent));
        }
    }
}

ProcessFilterProxy::ProcessFilterProxy(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setSortRole(ProcessModel::SortRole);
    setSortCaseSensitivity(Qt::CaseInsensitive);
    // Rows are re-sorted as values change rather than only on click, so a
    // CPU-sorted table keeps the busiest process at the top.
    setDynamicSortFilter(true);
    // A search that matches one child (typing a PID) still shows its group,
    // and a group that matches by name shows all of its children.
    setRecursiveFilteringEnabled(true);
    setAutoAcceptChildRows(true);
}

void ProcessFilterProxy::setSearchText(const QString &text)
{
    if (m_searchText == text) {
        return;
    }
    // begin/endFilterChange must bracket the criteria change; only rows are
    // filtered, so the column pass is skipped.
    beginFilterChange();
    m_searchText = text;
    endFilterChange(Direction::Rows);
}

void ProcessFilterProxy::setShowKernelThreads(bool show)
{
    if (m_showKernelThreads == show) {
        return;
    }
    beginFilterChange();
    m_showKernelThreads = show;
    endFilterChange(Direction::Rows);
}

bool ProcessFilterProxy::filterAcceptsRow(int sourceRow,
                                          const QModelIndex &sourceParent) const
{
    const QAbstractItemModel *source = sourceModel();
    if (!source) {
        return false;
    }

    const QModelIndex first = source->index(sourceRow, ProcessModel::Name,
                                            sourceParent);
    if (!m_showKernelThreads
        && first.data(ProcessModel::KernelThreadRole).toBool()) {
        return false;
    }

    if (m_searchText.isEmpty()) {
        return true;
    }

    // Match on the fields a user would actually search by, so typing a
    // number finds a PID and typing a path finds it in the command line.
    static constexpr ProcessModel::Column searched[] = {
        ProcessModel::Name,
        ProcessModel::Pid,
        ProcessModel::User,
        ProcessModel::Command,
    };
    for (const ProcessModel::Column column : searched) {
        // A group's Name is displayed with its member count appended, so
        // match the bare name instead, or typing "3" would find every group
        // of three.
        const int role = column == ProcessModel::Name ? ProcessModel::SortRole
                                                      : Qt::DisplayRole;
        const QString value = source->index(sourceRow, column, sourceParent)
                                  .data(role)
                                  .toString();
        if (value.contains(m_searchText, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}
