#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;

// Preferences, applied the moment they are changed rather than on an OK
// button: this is a live monitor, and the point of changing the update speed
// or the theme is to watch what it does.
//
// Modeless for the same reason — the graphs stay visible behind it.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;

private:
    QComboBox *m_interval;
    QComboBox *m_temperature;
    QComboBox *m_theme;
    QCheckBox *m_kernelThreads;
    QLabel *m_note;
};
