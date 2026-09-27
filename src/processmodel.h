#pragma once

#include <QAbstractItemModel>
#include <QSortFilterProxyModel>
#include <QVector>

#include <memory>
#include <utility>
#include <vector>

#include "providers/processprovider.h"

// Two-level model over the process list: processes that share a name are
// collected under one group row carrying their totals, and only the expanded
// children carry a PID. A name with a single process is shown as that
// process directly, with no group row around it.
//
// Updates are diffed against the previous snapshot rather than reset, at both
// levels, so selection, scroll position, sort order and which groups are
// expanded all survive each tick.
class ProcessModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Column {
        Name,
        Pid,
        User,
        Cpu,
        Memory,
        State,
        Nice,
        Threads,
        DiskRead,
        DiskWrite,
        GpuCompute,
        GpuVideo,
        Command,
        ColumnCount,
    };

    // Sorting must compare raw numbers, not the formatted strings, or "9.9%"
    // would sort above "10.0%" and "1 GiB" below "999 MiB".
    static constexpr int SortRole = Qt::UserRole;
    static constexpr int PidRole = Qt::UserRole + 1;
    static constexpr int KernelThreadRole = Qt::UserRole + 2;
    // Passed to the privileged helper so it can reject a reused PID.
    static constexpr int StartTicksRole = Qt::UserRole + 3;

    explicit ProcessModel(QObject *parent = nullptr);
    ~ProcessModel() override;

    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;

    void setSnapshot(const ProcessSnapshot &snapshot);

    // The single process an index stands for: a child row, or a top-level
    // row whose name has only one process. nullptr for a group row.
    const ProcessInfo *processAt(const QModelIndex &index) const;

    static QString columnTitle(Column column);
    // Niche columns are offered through the header menu but start hidden.
    // The GPU pair stays in that set even now that it is populated: on a
    // machine with no GPU, or one whose only card reports nothing
    // per-process, they would be a column of em dashes.
    static bool columnHiddenByDefault(Column column);

private:
    // Kernel threads are keyed apart from user processes, so a group is
    // either all kernel threads or none and the kernel-thread filter can
    // judge it as a whole.
    using GroupKey = std::pair<QString, bool>; // name, isKernelThread

    struct Group {
        GroupKey key;
        QVector<ProcessInfo> members; // ascending by pid
        // Whether the members are exposed as child rows. Kept apart from
        // members.size() because the diff passes through intermediate sizes
        // the view must not see: 3 -> 1 -> 3 is still a group throughout.
        bool hasChildren = false;
        int row = 0; // position in m_groups, for parent()

        // Totals, refreshed on every snapshot.
        double cpuPercent = 0.0;
        quint64 rssBytes = 0;
        int threads = 0;
        quint64 diskReadBytesPerSec = 0;
        quint64 diskWriteBytesPerSec = 0;
        bool ioAccessible = false; // for at least one member
        double gpuComputePercent = -1.0;
        double gpuVideoPercent = -1.0;
    };

    QVariant processData(const ProcessInfo &p, Column column, int role) const;
    QVariant groupData(const Group &group, Column column, int role) const;
    Group *groupOf(const QModelIndex &index) const;
    void syncMembers(Group &group, QVector<ProcessInfo> &incoming);
    void reindexGroups(int from);
    static void computeTotals(Group &group);

    // Ascending by key. Heap-allocated so that a child index can point at its
    // group and stay valid while groups are inserted and removed around it.
    std::vector<std::unique_ptr<Group>> m_groups;
};

// Search and sort in front of ProcessModel. Kernel threads are hidden by
// default: on a desktop they are numerous, unkillable noise. Filtering is
// recursive, so a group is shown when any of its processes matches.
class ProcessFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit ProcessFilterProxy(QObject *parent = nullptr);

    void setSearchText(const QString &text);
    void setShowKernelThreads(bool show);
    bool showKernelThreads() const { return m_showKernelThreads; }

protected:
    bool filterAcceptsRow(int sourceRow,
                          const QModelIndex &sourceParent) const override;

private:
    QString m_searchText;
    bool m_showKernelThreads = false;
};
