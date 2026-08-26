#pragma once

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QWidget>

#include "../providers/startupprovider.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeView;

// Table over the startup entry list. The data is static between scans, so
// this resets wholesale rather than diffing like ProcessModel does.
class StartupModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        Name,
        Kind,
        Status,
        Detail,
        ColumnCount,
    };

    static constexpr int SortRole = Qt::UserRole;
    static constexpr int EnabledRole = Qt::UserRole + 1;

    explicit StartupModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;

    void setEntries(const QVector<StartupEntry> &entries);
    const StartupEntry *entryAt(int row) const;

private:
    QVector<StartupEntry> m_rows;
};

// Startup Apps tab: everything that runs at login, from XDG autostart files
// and systemd user units, with a switch for each.
//
// This is the only tab that writes state outside the app's own config, so
// every failure is surfaced rather than swallowed, and the list is re-scanned
// after each change so the display can never drift from what is on disk.
class StartupPage : public QWidget
{
    Q_OBJECT

public:
    explicit StartupPage(QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void applyTheme();
    void refresh();
    void toggleSelected();
    void updateButton();
    const StartupEntry *selectedEntry() const;

    StartupProvider m_provider;
    StartupModel *m_model;
    QSortFilterProxyModel *m_proxy;
    QTreeView *m_view;
    QLineEdit *m_search;
    QPushButton *m_toggleButton;
    QLabel *m_summary;
};
