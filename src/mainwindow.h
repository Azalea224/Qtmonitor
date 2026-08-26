#pragma once

#include <QMainWindow>

class QListWidget;
class QStackedWidget;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void addPage(const QString &title, const QString &iconName, QWidget *page);
    void restoreSettings();
    void saveSettings();

    QListWidget *m_nav;
    QStackedWidget *m_pages;
};
