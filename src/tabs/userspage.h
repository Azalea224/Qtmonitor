#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QVector>
#include <QWidget>

#include "../providers/processprovider.h"
#include "../providers/sessionprovider.h"

class QLabel;
class QTreeView;
class Sampler;

// Two-level model: users at the top with their totals, their processes
// underneath. Mirrors how Task Manager presents the same information.
//
// Rows are diffed rather than reset, for the reason ProcessModel is: a full
// reset every second would collapse whichever user the reader had expanded
// and throw away their selection.
class UsersModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Column {
        Name,
        Pid,
        Status,
        Cpu,
        Memory,
        ColumnCount,
    };

    static constexpr int SortRole = Qt::UserRole;
    static constexpr int PidRole = Qt::UserRole + 1;

    explicit UsersModel(QObject *parent = nullptr);

    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;

    void update(const ProcessSnapshot &snapshot,
                const QHash<uint, QVector<UserSession>> &sessions);

    int userCount() const { return m_users.size(); }
    QString userNameAt(int row) const;

private:
    struct UserGroup {
        uint uid = 0;
        QString name;
        QString sessionSummary;
        double cpuPercent = 0.0;
        quint64 rssBytes = 0;
        QVector<ProcessInfo> processes; // ascending by pid
    };

    // internalId is 0 for a user row, or the owning user's index + 1 for a
    // process row, which is what makes parent() cheap.
    static constexpr quintptr kUserLevel = 0;

    void syncProcesses(int userRow, QVector<ProcessInfo> &incoming);

    QVector<UserGroup> m_users;
};

// Users tab: who is logged in, how they are logged in, and what their
// processes are costing.
class UsersPage : public QWidget
{
    Q_OBJECT

public:
    explicit UsersPage(Sampler *sampler, QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;

private:
    void applyTheme();
    void onProcessesSampled(const ProcessSnapshot &snapshot);

    SessionProvider m_sessions;
    UsersModel *m_model;
    QTreeView *m_view;
    QLabel *m_summary;

    QHash<uint, QVector<UserSession>> m_cachedSessions;
    qint64 m_sessionsFetchedMsec = 0;

    // Sessions change on the timescale of logins, not seconds.
    static constexpr qint64 kSessionRefreshMsec = 5000;
};
