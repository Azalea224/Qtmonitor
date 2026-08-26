#pragma once

#include <QWidget>

// Stand-in tab content until the phase that implements the real page.
// Displays which development phase will deliver this tab.
class PlaceholderPage : public QWidget
{
    Q_OBJECT

public:
    explicit PlaceholderPage(int phase, QWidget *parent = nullptr);
};
