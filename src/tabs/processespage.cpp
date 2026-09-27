#include "processespage.h"

#include <QAction>
#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPoint>
#include <QPushButton>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

#include <KConfigGroup>
#include <KSharedConfig>

#include "../processmodel.h"
#include "../providers/processcontrol.h"
#include "../sampler.h"
#include "../settings.h"
#include "theming.h"

namespace {
constexpr char kConfigGroup[] = "Processes";
}

ProcessesPage::ProcessesPage(Sampler *sampler, QWidget *parent)
    : QWidget(parent)
    , m_model(new ProcessModel(this))
    , m_proxy(new ProcessFilterProxy(this))
    , m_view(new QTreeView(this))
    , m_search(new QLineEdit(this))
    , m_kernelThreads(new QCheckBox(tr("Kernel threads"), this))
    , m_endButton(new QPushButton(tr("End process"), this))
    , m_summary(new QLabel(this))
{
    const QVector<GpuDeviceInfo> gpus = sampler->gpus().devices();
    m_gpuMetricsAvailable =
        std::any_of(gpus.cbegin(), gpus.cend(),
                    [](const GpuDeviceInfo &gpu) { return gpu.metricsAvailable; });

    m_proxy->setSourceModel(m_model);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *toolbar = new QHBoxLayout;
    m_search->setPlaceholderText(
        tr("Search by name, PID, user or command line"));
    m_search->setClearButtonEnabled(true);
    toolbar->addWidget(m_search, 1);
    toolbar->addWidget(m_kernelThreads);
    toolbar->addWidget(m_endButton);
    layout->addLayout(toolbar);

    m_view->setModel(m_proxy);
    // Processes sharing a name are grouped under an expandable total row.
    m_view->setRootIsDecorated(true);
    m_view->setUniformRowHeights(true); // lets the view skip per-row sizing
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setSortingEnabled(true);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->sortByColumn(ProcessModel::Cpu, Qt::DescendingOrder);
    m_view->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->header()->setSectionsMovable(true);
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(QHeaderView::Interactive);
    layout->addWidget(m_view, 1);

    layout->addWidget(m_summary);
    applyTheme();

    m_endButton->setEnabled(false);

    connect(m_search, &QLineEdit::textChanged,
            m_proxy, &ProcessFilterProxy::setSearchText);
    connect(m_kernelThreads, &QCheckBox::toggled,
            m_proxy, &ProcessFilterProxy::setShowKernelThreads);
    connect(m_endButton, &QPushButton::clicked,
            this, &ProcessesPage::endSelectedProcess);
    connect(m_view, &QWidget::customContextMenuRequested,
            this, &ProcessesPage::showRowMenu);
    connect(m_view->header(), &QWidget::customContextMenuRequested,
            this, &ProcessesPage::showHeaderMenu);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this] { m_endButton->setEnabled(selectedPid() > 0); });
    // On a group row, double-click keeps its default job of expanding it.
    connect(m_view, &QAbstractItemView::doubleClicked, this, [this] {
        if (selectedPid() > 0) {
            m_endButton->setFocus();
        }
    });

    connect(sampler, &Sampler::processesSampled,
            this, &ProcessesPage::onProcessesSampled);

    // The same preference is also offered in the settings dialog, so the two
    // controls are bound to the one stored value instead of to each other.
    connect(m_kernelThreads, &QCheckBox::toggled, &Settings::instance(),
            &Settings::setShowKernelThreads);
    connect(&Settings::instance(), &Settings::showKernelThreadsChanged,
            m_kernelThreads, &QCheckBox::setChecked);
    m_kernelThreads->setChecked(Settings::instance().showKernelThreads());

    // Must come after the widget connections: restoring a checked "kernel
    // threads" box has to propagate into the proxy, and setChecked() only
    // does that once the toggled signal is wired up.
    restoreColumnState();

    // Applied after restoreState(), which would otherwise overwrite it. Name
    // soaks up the leftover width so the table has no dead space on the
    // right whichever columns are enabled.
    m_view->header()->setSectionResizeMode(ProcessModel::Name,
                                           QHeaderView::Stretch);
}

ProcessesPage::~ProcessesPage()
{
    saveColumnState();
}

void ProcessesPage::applyTheme()
{
    theming::markSecondary(m_summary, palette());
}

void ProcessesPage::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

void ProcessesPage::restoreColumnState()
{
    const KConfigGroup group(KSharedConfig::openConfig(),
                             QLatin1String(kConfigGroup));

    const QByteArray state = group.readEntry("headerState", QByteArray());
    // restoreState() also carries hidden sections, widths, visual order and
    // the sort indicator, so it covers the whole column configuration.
    if (!state.isEmpty() && m_view->header()->restoreState(state)) {
        return;
    }

    for (int column = 0; column < ProcessModel::ColumnCount; ++column) {
        const auto typed = static_cast<ProcessModel::Column>(column);
        m_view->setColumnHidden(column, ProcessModel::columnHiddenByDefault(typed));
        m_view->setColumnWidth(column, typed == ProcessModel::Name ? 260 : 110);
    }
}

void ProcessesPage::saveColumnState()
{
    KConfigGroup group(KSharedConfig::openConfig(), QLatin1String(kConfigGroup));
    group.writeEntry("headerState", m_view->header()->saveState());
    group.sync();
}

void ProcessesPage::onProcessesSampled(const ProcessSnapshot &snapshot)
{
    m_model->setSnapshot(snapshot);

    // Count processes, not rows: a group row stands for all of its visible
    // children.
    int shown = 0;
    for (int row = 0; row < m_proxy->rowCount(); ++row) {
        const int children = m_proxy->rowCount(m_proxy->index(row, 0));
        shown += children > 0 ? children : 1;
    }
    QString text = tr("%n process(es)", "", shown);
    if (!m_kernelThreads->isChecked() && snapshot.kernelThreadCount > 0) {
        text += QStringLiteral("  ·  ")
            + tr("%n kernel thread(s) hidden", "", snapshot.kernelThreadCount);
    }
    m_summary->setText(text);
}

int ProcessesPage::selectedPid() const
{
    const QModelIndex index = m_view->currentIndex();
    if (!index.isValid()) {
        return -1;
    }
    return index.data(ProcessModel::PidRole).toInt();
}

QString ProcessesPage::selectedName() const
{
    const QModelIndex index = m_view->currentIndex();
    if (!index.isValid()) {
        return {};
    }
    return index.siblingAtColumn(ProcessModel::Name).data(Qt::DisplayRole).toString();
}

quint64 ProcessesPage::selectedStartTicks() const
{
    const QModelIndex index = m_view->currentIndex();
    if (!index.isValid()) {
        return 0;
    }
    return index.data(ProcessModel::StartTicksRole).toULongLong();
}

void ProcessesPage::escalate(int pid, quint64 startTicks, const QString &name,
                             processcontrol::Signal signal)
{
    const QString signalName = processcontrol::signalName(signal);

    if (!processcontrol::privilegedBackendAvailable()) {
        QMessageBox::information(
            this, tr("Authentication unavailable"),
            tr("%1 belongs to another user, so signalling it requires "
               "authorization.\n\nQtmonitor is not installed system-wide, so "
               "the privileged helper and its polkit policy are unavailable. "
               "Install the package to enable this.")
                .arg(name));
        return;
    }

    // No extra confirmation here: the user already confirmed the signal, and
    // polkit's own dialog states which action is being authorized. Asking
    // twice before the password prompt is just dialog fatigue.
    processcontrol::sendPrivileged(
        pid, signal, startTicks, this,
        [this, name, pid, signalName](const processcontrol::PrivilegedOutcome &outcome) {
            if (outcome.succeeded) {
                return; // the row disappears on the next sample
            }
            if (!outcome.authorized) {
                // Cancelling is a deliberate choice, not an error worth a
                // warning dialog.
                return;
            }
            QMessageBox::warning(
                this, tr("Could not signal process"),
                outcome.message.isEmpty()
                    ? tr("Sending %1 to %2 (PID %3) failed.")
                          .arg(signalName, name).arg(pid)
                    : outcome.message);
        });
}

void ProcessesPage::showHeaderMenu(const QPoint &pos)
{
    QMenu menu(this);
    menu.addSection(tr("Columns"));

    for (int column = 0; column < ProcessModel::ColumnCount; ++column) {
        const auto typed = static_cast<ProcessModel::Column>(column);
        QString title = ProcessModel::columnTitle(typed);
        // Both GPU columns start hidden, so say up front when enabling one
        // would only produce a column of em dashes.
        if (!m_gpuMetricsAvailable
            && (typed == ProcessModel::GpuCompute || typed == ProcessModel::GpuVideo)) {
            title += tr(" (no GPU detected)");
        }
        QAction *action = menu.addAction(title);
        action->setCheckable(true);
        action->setChecked(!m_view->isColumnHidden(column));
        // The Name column is the row's identity; hiding it leaves an
        // unreadable table.
        action->setEnabled(typed != ProcessModel::Name);
        connect(action, &QAction::toggled, this, [this, column](bool visible) {
            m_view->setColumnHidden(column, !visible);
        });
    }

    menu.exec(m_view->header()->mapToGlobal(pos));
}

void ProcessesPage::showRowMenu(const QPoint &pos)
{
    const int pid = selectedPid();
    if (pid <= 0) {
        return;
    }

    QMenu menu(this);
    // Real signal names, not a euphemism: the distinction is meaningful.
    QAction *term = menu.addAction(tr("End process (SIGTERM)"));
    QAction *kill = menu.addAction(tr("Kill process (SIGKILL)"));

    const QAction *chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));
    if (chosen == term) {
        endSelectedProcess();
    } else if (chosen == kill) {
        const QString name = selectedName();
        const quint64 startTicks = selectedStartTicks();
        if (QMessageBox::warning(
                this, tr("Kill process"),
                tr("Send SIGKILL to %1 (PID %2)?\n\nThe process is terminated "
                   "immediately and cannot save its work or clean up.")
                    .arg(name).arg(pid),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
            != QMessageBox::Yes) {
            return;
        }
        const processcontrol::Result result =
            processcontrol::send(pid, processcontrol::Signal::Kill);
        if (result == processcontrol::Result::NotPermitted) {
            escalate(pid, startTicks, name, processcontrol::Signal::Kill);
        } else if (result != processcontrol::Result::Ok) {
            QMessageBox::warning(this, tr("Kill process"),
                                 processcontrol::describe(result));
        }
    }
}

void ProcessesPage::endSelectedProcess()
{
    const int pid = selectedPid();
    const QString name = selectedName();
    const quint64 startTicks = selectedStartTicks();
    if (pid <= 0) {
        return;
    }

    if (QMessageBox::question(
            this, tr("End process"),
            tr("Send SIGTERM to %1 (PID %2)?\n\nThe process is asked to shut "
               "down and may save its work first.")
                .arg(name).arg(pid),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
        != QMessageBox::Yes) {
        return;
    }

    const processcontrol::Result result =
        processcontrol::send(pid, processcontrol::Signal::Term);

    if (result == processcontrol::Result::NotPermitted) {
        escalate(pid, startTicks, name, processcontrol::Signal::Term);
        return;
    }
    if (result != processcontrol::Result::Ok) {
        QMessageBox::warning(this, tr("End process"),
                             processcontrol::describe(result));
        return;
    }

    requestKillAfterTerm(pid, name);
}

void ProcessesPage::requestKillAfterTerm(int pid, const QString &name)
{
    // `this` as the context object cancels the callback if the page goes
    // away while the grace period is running.
    QTimer::singleShot(kTermGraceMsec, this, [this, pid, name] {
        if (!processcontrol::exists(pid)) {
            return; // SIGTERM worked
        }
        if (processcontrol::isZombie(pid)) {
            // SIGKILL would be silently useless here, so say what is
            // actually going on instead of offering it.
            QMessageBox::information(
                this, tr("Process already exited"),
                tr("%1 (PID %2) has exited but is still listed as a zombie, "
                   "because its parent has not collected its exit status.\n\n"
                   "It uses no CPU or memory and cannot be killed — it "
                   "disappears when the parent reaps it or itself exits.")
                    .arg(name).arg(pid));
            return;
        }
        if (QMessageBox::question(
                this, tr("Process still running"),
                tr("%1 (PID %2) has not exited %3 seconds after SIGTERM.\n\n"
                   "Send SIGKILL to force it to stop?")
                    .arg(name).arg(pid).arg(kTermGraceMsec / 1000),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes) {
            return;
        }
        const processcontrol::Result result =
            processcontrol::send(pid, processcontrol::Signal::Kill);
        if (result != processcontrol::Result::Ok
            && result != processcontrol::Result::NoSuchProcess) {
            QMessageBox::warning(this, tr("Kill process"),
                                 processcontrol::describe(result));
        }
    });
}
