#pragma once

#include <QWidget>

#include "../providers/processcontrol.h"
#include "../providers/processprovider.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPoint;
class QPushButton;
class QTreeView;
class ProcessModel;
class ProcessFilterProxy;
class Sampler;

// Processes tab: searchable, sortable process list with configurable
// columns and signal-based process termination. Processes that share a name
// are grouped under one expandable row showing their totals.
class ProcessesPage : public QWidget
{
    Q_OBJECT

public:
    explicit ProcessesPage(Sampler *sampler, QWidget *parent = nullptr);
    ~ProcessesPage() override;

protected:
    void changeEvent(QEvent *event) override;

private:
    void onProcessesSampled(const ProcessSnapshot &snapshot);
    void showHeaderMenu(const QPoint &pos);
    void showRowMenu(const QPoint &pos);
    void endSelectedProcess();
    void requestKillAfterTerm(int pid, const QString &name);
    // Offers the polkit route for a process this session does not own.
    void escalate(int pid, quint64 startTicks, const QString &name,
                  processcontrol::Signal signal);
    int selectedPid() const;
    QString selectedName() const;
    quint64 selectedStartTicks() const;
    void applyTheme();
    void restoreColumnState();
    void saveColumnState();

    ProcessModel *m_model;
    ProcessFilterProxy *m_proxy;
    QTreeView *m_view;
    QLineEdit *m_search;
    QCheckBox *m_kernelThreads;
    QPushButton *m_endButton;
    QLabel *m_summary;
    // Whether any GPU backend can attribute usage to processes. Decides
    // whether the two GPU columns are offered plainly or flagged as dead.
    bool m_gpuMetricsAvailable = false;

    // Seconds a SIGTERM is given to take effect before SIGKILL is offered.
    static constexpr int kTermGraceMsec = 3000;
};
