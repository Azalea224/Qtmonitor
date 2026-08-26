#include "placeholderpage.h"

#include <QLabel>
#include <QVBoxLayout>

PlaceholderPage::PlaceholderPage(int phase, QWidget *parent)
    : QWidget(parent)
{
    auto *label = new QLabel(
        QStringLiteral("This tab is implemented in Phase %1.").arg(phase), this);
    label->setAlignment(Qt::AlignCenter);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(label);
}
