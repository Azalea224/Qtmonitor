#include <QApplication>
#include <QIcon>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("qtmonitor"));
    QCoreApplication::setApplicationName(QStringLiteral("qtmonitor"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    // Respect the DE's active icon theme; fall back to our bundled icon so the
    // app never renders with a missing icon on minimal themes.
    const QIcon fallback(QStringLiteral(":/icons/qtmonitor.svg"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("qtmonitor"), fallback));

    MainWindow window;
    window.show();

    return app.exec();
}
