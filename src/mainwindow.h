#pragma once

#include <QMainWindow>
#include <QPointer>

class QTabWidget;
class Sampler;
class SettingsDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void restoreSettings();
    void saveSettings();
    void openSettings();

    QTabWidget *m_tabs;
    Sampler *m_sampler;
    // Modeless, so it is kept and re-shown rather than rebuilt each time.
    QPointer<SettingsDialog> m_settingsDialog;
};
