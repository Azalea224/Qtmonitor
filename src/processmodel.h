#pragma once

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QVector>

#include "providers/processprovider.h"

// Table model over the process list. Updates are diffed against the previous
// snapshot rather than reset, so selection, scroll position and sort order
// survive each tick.
class ProcessModel : public QAbstractTableModel
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

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;

    void setSnapshot(const ProcessSnapshot &snapshot);

    // Row lookup for the UI; returns nullptr if the row is out of range.
    const ProcessInfo *processAt(int row) const;

    static QString columnTitle(Column column);
    // Niche columns are offered through the header menu but start hidden.
    // The GPU pair stays in that set even now that it is populated: on a
    // machine with no GPU, or one whose only card reports nothing
    // per-process, they would be a column of em dashes.
    static bool columnHiddenByDefault(Column column);

private:
    QVector<ProcessInfo> m_rows; // ascending by pid, mirrors the snapshot
};

// Search and sort in front of ProcessModel. Kernel threads are hidden by
// default: on a desktop they are numerous, unkillable noise.
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
