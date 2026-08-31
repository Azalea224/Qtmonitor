#include "processmodel.h"

#include <QCoreApplication>
#include <QSet>

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
    : QAbstractTableModel(parent)
{
}

int ProcessModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int ProcessModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

const ProcessInfo *ProcessModel::processAt(int row) const
{
    if (row < 0 || row >= m_rows.size()) {
        return nullptr;
    }
    return &m_rows.at(row);
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
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return {};
    }

    const ProcessInfo &p = m_rows.at(index.row());
    const auto column = static_cast<Column>(index.column());

    if (role == PidRole) {
        return p.pid;
    }
    if (role == KernelThreadRole) {
        return p.isKernelThread;
    }
    if (role == StartTicksRole) {
        return p.startTicks;
    }

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

void ProcessModel::setSnapshot(const ProcessSnapshot &snapshot)
{
    const QVector<ProcessInfo> &incoming = snapshot.processes;

    if (m_rows.isEmpty()) {
        if (incoming.isEmpty()) {
            return;
        }
        beginInsertRows(QModelIndex(), 0, incoming.size() - 1);
        m_rows = incoming;
        endInsertRows();
        return;
    }

    // Both lists are sorted by pid. Remove the exited processes first, which
    // leaves m_rows as a subsequence of the incoming list.
    QSet<int> livePids;
    livePids.reserve(incoming.size());
    for (const ProcessInfo &p : incoming) {
        livePids.insert(p.pid);
    }

    for (int row = m_rows.size() - 1; row >= 0; --row) {
        if (livePids.contains(m_rows.at(row).pid)) {
            continue;
        }
        const int last = row;
        while (row > 0 && !livePids.contains(m_rows.at(row - 1).pid)) {
            --row;
        }
        beginRemoveRows(QModelIndex(), row, last);
        m_rows.remove(row, last - row + 1);
        endRemoveRows();
    }

    // Walk both lists together: matching pids are refreshed in place, and
    // runs of new pids are inserted at the position that keeps pid order.
    int row = 0;
    for (int j = 0; j < incoming.size(); ++j) {
        if (row < m_rows.size() && m_rows.at(row).pid == incoming.at(j).pid) {
            m_rows[row] = incoming.at(j);
            ++row;
            continue;
        }
        const int runStart = j;
        while (j < incoming.size()
               && (row >= m_rows.size()
                   || m_rows.at(row).pid != incoming.at(j).pid)) {
            ++j;
        }
        const int count = j - runStart;
        beginInsertRows(QModelIndex(), row, row + count - 1);
        for (int k = 0; k < count; ++k) {
            m_rows.insert(row + k, incoming.at(runStart + k));
        }
        endInsertRows();
        row += count;
        --j; // the outer ++j re-examines the row that stopped the run
    }

    if (!m_rows.isEmpty()) {
        // One coarse notification: the view only repaints visible rows, so
        // this is cheaper than tracking which cells actually moved.
        emit dataChanged(index(0, 0), index(m_rows.size() - 1, ColumnCount - 1));
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
        const QString value = source->index(sourceRow, column, sourceParent)
                                  .data(Qt::DisplayRole)
                                  .toString();
        if (value.contains(m_searchText, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}
