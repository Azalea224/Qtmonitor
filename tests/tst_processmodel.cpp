// ProcessModel tests: grouping by name, the group totals, and the two-level
// diff that keeps expanded groups and selections alive across ticks.
//
// Every test runs under QAbstractItemModelTester, which checks the model's
// structural invariants (parent/index round trips, row counts announced by
// begin/end signals) after every change. The diff is where a mistake would
// otherwise only show up as a crash in the view.

#include <QAbstractItemModelTester>
#include <QPersistentModelIndex>
#include <QTest>

#include "processmodel.h"

namespace {

ProcessInfo process(int pid, const QString &name, double cpu = 0.0,
                    quint64 rss = 0, uint uid = 1000)
{
    ProcessInfo p;
    p.pid = pid;
    p.name = name;
    p.command = name;
    p.uid = uid;
    p.user = uid == 0 ? QStringLiteral("root") : QStringLiteral("user");
    p.cpuPercent = cpu;
    p.rssBytes = rss;
    p.threads = 1;
    p.state = QLatin1Char('S');
    return p;
}

ProcessSnapshot snapshot(QVector<ProcessInfo> processes)
{
    std::sort(processes.begin(), processes.end(),
              [](const ProcessInfo &a, const ProcessInfo &b) { return a.pid < b.pid; });
    ProcessSnapshot s;
    s.processes = processes;
    return s;
}

// Top-level row for a name, found by scanning; -1 if absent.
int rowOf(const QAbstractItemModel &model, const QString &name)
{
    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.index(row, ProcessModel::Name).data(ProcessModel::SortRole)
            == name.toLower()) {
            return row;
        }
    }
    return -1;
}

} // namespace

class TestProcessModel : public QObject
{
    Q_OBJECT

private slots:
    void groupsBySharedName();
    void singleProcessIsNotAGroup();
    void unmeasuredMembersAreLeftOutOfTotals();
    void kernelThreadsGroupApart();
    void groupsGrowAndShrink();
    void childIndexSurvivesGroupsMovingAroundIt();
    void searchByPidShowsItsGroup();
    void groupWithEveryPidReplaced();
};

void TestProcessModel::groupsBySharedName()
{
    ProcessModel model;
    QAbstractItemModelTester tester(&model);
    model.setSnapshot(snapshot({
        process(10, QStringLiteral("firefox"), 2.0, 100),
        process(11, QStringLiteral("firefox"), 3.0, 200),
        process(12, QStringLiteral("firefox"), 0.5, 50),
        process(20, QStringLiteral("foot"), 1.0, 10),
    }));

    QCOMPARE(model.rowCount(), 2);
    const QModelIndex group = model.index(rowOf(model, QStringLiteral("firefox")), 0);
    QCOMPARE(model.rowCount(group), 3);

    // The group row carries totals and no PID of its own...
    QCOMPARE(group.data(ProcessModel::PidRole).toInt(), 0);
    QCOMPARE(group.siblingAtColumn(ProcessModel::Pid).data().toString(), QString());
    QCOMPARE(group.siblingAtColumn(ProcessModel::Cpu).data(ProcessModel::SortRole).toDouble(), 5.5);
    QCOMPARE(group.siblingAtColumn(ProcessModel::Memory).data(ProcessModel::SortRole).toULongLong(),
             quint64(350));
    QCOMPARE(group.siblingAtColumn(ProcessModel::Threads).data().toInt(), 3);

    // ...and the PIDs live only on the children.
    QCOMPARE(model.index(0, ProcessModel::Pid, group).data().toInt(), 10);
    QCOMPARE(model.index(2, ProcessModel::Pid, group).data(ProcessModel::PidRole).toInt(), 12);
}

void TestProcessModel::singleProcessIsNotAGroup()
{
    ProcessModel model;
    QAbstractItemModelTester tester(&model);
    model.setSnapshot(snapshot({process(20, QStringLiteral("foot"), 1.0, 10)}));

    const QModelIndex row = model.index(0, 0);
    QCOMPARE(model.rowCount(row), 0);
    QCOMPARE(row.data().toString(), QStringLiteral("foot"));
    QCOMPARE(row.data(ProcessModel::PidRole).toInt(), 20);
}

void TestProcessModel::unmeasuredMembersAreLeftOutOfTotals()
{
    ProcessInfo measured = process(1, QStringLiteral("app"));
    measured.gpuComputePercent = 12.0;
    measured.ioAccessible = true;
    measured.diskReadBytesPerSec = 4096;
    ProcessInfo unmeasured = process(2, QStringLiteral("app"));
    unmeasured.diskReadBytesPerSec = 999; // unreadable, so must not count
    ProcessInfo alsoUnmeasured = process(3, QStringLiteral("other"));
    ProcessInfo alsoUnmeasured2 = process(4, QStringLiteral("other"));

    ProcessModel model;
    QAbstractItemModelTester tester(&model);
    model.setSnapshot(snapshot({measured, unmeasured, alsoUnmeasured, alsoUnmeasured2}));

    const QModelIndex app = model.index(rowOf(model, QStringLiteral("app")), 0);
    QCOMPARE(app.siblingAtColumn(ProcessModel::GpuCompute).data(ProcessModel::SortRole).toDouble(), 12.0);
    QCOMPARE(app.siblingAtColumn(ProcessModel::DiskRead).data(ProcessModel::SortRole).toULongLong(),
             quint64(4096));

    // No member measured: the total is unknown, not zero.
    const QModelIndex other = model.index(rowOf(model, QStringLiteral("other")), 0);
    QCOMPARE(other.siblingAtColumn(ProcessModel::GpuCompute).data().toString(), QStringLiteral("—"));
    QCOMPARE(other.siblingAtColumn(ProcessModel::DiskRead).data().toString(), QStringLiteral("—"));
}

void TestProcessModel::kernelThreadsGroupApart()
{
    ProcessInfo kthread = process(2, QStringLiteral("worker"), 0.0, 0, 0);
    kthread.isKernelThread = true;

    ProcessModel model;
    QAbstractItemModelTester tester(&model);
    model.setSnapshot(snapshot({kthread, process(50, QStringLiteral("worker"))}));

    QCOMPARE(model.rowCount(), 2);
    ProcessFilterProxy proxy;
    proxy.setSourceModel(&model);
    QCOMPARE(proxy.rowCount(), 1);
    QCOMPARE(proxy.index(0, 0).data(ProcessModel::PidRole).toInt(), 50);
}

void TestProcessModel::groupsGrowAndShrink()
{
    ProcessModel model;
    QAbstractItemModelTester tester(&model);
    const QString name = QStringLiteral("bash");

    model.setSnapshot(snapshot({process(5, name)}));
    QCOMPARE(model.rowCount(model.index(0, 0)), 0);

    model.setSnapshot(snapshot({process(5, name), process(9, name)}));
    QCOMPARE(model.rowCount(model.index(0, 0)), 2);

    // Churn inside a group: 5 exits, 7 and 11 arrive.
    model.setSnapshot(snapshot({process(7, name), process(9, name), process(11, name)}));
    const QModelIndex group = model.index(0, 0);
    QCOMPARE(model.rowCount(group), 3);
    QCOMPARE(model.index(0, ProcessModel::Pid, group).data().toInt(), 7);
    QCOMPARE(model.index(2, ProcessModel::Pid, group).data().toInt(), 11);

    // Down to one member, which also changes which process it is.
    model.setSnapshot(snapshot({process(11, name)}));
    QCOMPARE(model.rowCount(model.index(0, 0)), 0);
    QCOMPARE(model.index(0, 0).data(ProcessModel::PidRole).toInt(), 11);

    model.setSnapshot(snapshot({}));
    QCOMPARE(model.rowCount(), 0);
}

void TestProcessModel::childIndexSurvivesGroupsMovingAroundIt()
{
    ProcessModel model;
    QAbstractItemModelTester tester(&model);
    model.setSnapshot(snapshot({
        process(30, QStringLiteral("m")),
        process(40, QStringLiteral("z")),
        process(41, QStringLiteral("z")),
    }));

    const QModelIndex zGroup = model.index(rowOf(model, QStringLiteral("z")), 0);
    const QPersistentModelIndex child = model.index(1, ProcessModel::Pid, zGroup);
    QCOMPARE(child.data().toInt(), 41);

    // A group appears before z and another before that disappears, so z's
    // row moves twice while the child index is held.
    model.setSnapshot(snapshot({
        process(10, QStringLiteral("a")),
        process(11, QStringLiteral("b")),
        process(40, QStringLiteral("z")),
        process(41, QStringLiteral("z")),
    }));

    QVERIFY(child.isValid());
    QCOMPARE(child.data().toInt(), 41);
    QCOMPARE(child.parent().row(), rowOf(model, QStringLiteral("z")));
    QCOMPARE(child.parent().row(), 2);
}

void TestProcessModel::searchByPidShowsItsGroup()
{
    ProcessModel model;
    ProcessFilterProxy proxy;
    proxy.setSourceModel(&model);
    QAbstractItemModelTester tester(&proxy);
    model.setSnapshot(snapshot({
        process(100, QStringLiteral("chromium")),
        process(1234, QStringLiteral("chromium")),
        process(300, QStringLiteral("foot")),
    }));

    proxy.setSearchText(QStringLiteral("1234"));
    QCOMPARE(proxy.rowCount(), 1);
    const QModelIndex group = proxy.index(0, 0);
    QCOMPARE(proxy.rowCount(group), 1);
    QCOMPARE(proxy.index(0, 0, group).data(ProcessModel::PidRole).toInt(), 1234);

    // Matching the name shows the whole group, and the member count in the
    // displayed name is not searchable text.
    proxy.setSearchText(QStringLiteral("chrom"));
    QCOMPARE(proxy.rowCount(proxy.index(0, 0)), 2);
    proxy.setSearchText(QStringLiteral("(2)"));
    QCOMPARE(proxy.rowCount(), 0);
}

void TestProcessModel::groupWithEveryPidReplaced()
{
    // nvidia-smi is re-spawned every tick, so its group keeps its name while
    // no pid survives. Mid-diff the group is briefly empty, and the recursive
    // proxy re-filters it right then — this crashed the app on start.
    ProcessModel model;
    ProcessFilterProxy proxy;
    proxy.setSourceModel(&model);
    QAbstractItemModelTester tester(&proxy);
    const QString name = QStringLiteral("nvidia-smi");

    model.setSnapshot(snapshot({process(100, name), process(101, name)}));
    model.setSnapshot(snapshot({process(200, name), process(201, name)}));

    QCOMPARE(proxy.rowCount(), 1);
    const QModelIndex group = proxy.index(0, 0);
    QCOMPARE(proxy.rowCount(group), 2);
    QCOMPARE(proxy.index(0, 0, group).data(ProcessModel::PidRole).toInt(), 200);
}

QTEST_MAIN(TestProcessModel)
#include "tst_processmodel.moc"
