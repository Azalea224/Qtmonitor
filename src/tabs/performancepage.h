#pragma once

#include <QHash>
#include <QVector>
#include <QWidget>

#include "../providers/cpuprovider.h"
#include "../providers/diskprovider.h"
#include "../providers/gpuprovider.h"
#include "../providers/memoryprovider.h"
#include "../providers/networkprovider.h"

class QLabel;
class QListWidget;
class QStackedWidget;
class MiniGraph;
class Sampler;

// Sidebar row widget: resource name above a live MiniGraph sparkline of
// overall utilization (no icon).
class ResourceNavRow : public QWidget
{
    Q_OBJECT

public:
    // Takes the list explicitly, not just as a parent: setItemWidget reparents
    // the row to the viewport, and the row still needs to know whether the
    // list has focus to tint itself correctly.
    ResourceNavRow(const QString &title, int historySeconds, QListWidget *list);
    void pushValue(double percent);

    // Throughput rows plot against their own rolling peak instead of 0..100;
    // see MiniGraph::setAutoScale.
    void setAutoScale(bool enabled);

    // The list paints its Highlight behind this widget, but a child label
    // keeps drawing in WindowText, which on a light highlight is barely
    // legible. So the row is told when it is the current one and re-tints.
    void setSelected(bool selected);

protected:
    void changeEvent(QEvent *event) override;
    // Watches the list for focus changes, which switch the color group the
    // view paints the selection with.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyTheme();

    QListWidget *m_list;
    QLabel *m_name;
    MiniGraph *m_graph;
    bool m_selected = false;
};

// Performance tab: resource sidebar (CPU, Memory, one row per GPU, per disk
// and per network interface) with a stacked content pane, Mission Center /
// Task Manager style. Each sidebar row shows a live sparkline of that
// resource's overall usage.
class PerformancePage : public QWidget
{
    Q_OBJECT

public:
    explicit PerformancePage(Sampler *sampler, QWidget *parent = nullptr);

private:
    enum class ResourceKind { Cpu, Memory, Gpu, Disk, Network };

    void addGpuResources(Sampler *sampler);
    void addDiskResources(Sampler *sampler);
    void addNetworkResources(Sampler *sampler);
    ResourceNavRow *addResource(const QString &title, ResourceKind kind, QWidget *page);
    void onCpuSample(const CpuSnapshot &snapshot);
    void onMemorySample(const MemorySnapshot &snapshot);
    void onGpuSample(const QVector<GpuSnapshot> &snapshots);
    void onDiskSample(const QVector<DiskSnapshot> &snapshots);
    void onNetworkSample(const QVector<NetworkSnapshot> &snapshots);

    QListWidget *m_nav;
    QStackedWidget *m_stack;
    // In nav order, so the current row can be re-tinted on selection change.
    QVector<ResourceNavRow *> m_rows;
    ResourceNavRow *m_cpuRow;
    ResourceNavRow *m_memRow;
    // Keyed by PCI address, the identity every GPU backend agrees on.
    QHash<QString, ResourceNavRow *> m_gpuRows;
    // Both keyed by kernel name, the identity /proc and /sys agree on.
    QHash<QString, ResourceNavRow *> m_diskRows;
    QHash<QString, ResourceNavRow *> m_networkRows;

    static constexpr int kHistorySeconds = 60;
};
