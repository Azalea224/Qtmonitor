#include <QApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

#include "mainwindow.h"
#include "settings.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("qtmonitor"));
    QCoreApplication::setApplicationName(QStringLiteral("qtmonitor"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    // Names the window to the desktop, by two different mechanisms.
    //
    // On Wayland this becomes the xdg-toplevel app_id verbatim, so it has to
    // be the desktop entry's basename for a taskbar to pair the window with
    // its launcher. On X11 it is ignored: Qt builds WM_CLASS from
    // applicationName above, which is why the desktop entry also carries
    // StartupWMClass=qtmonitor.
    QGuiApplication::setDesktopFileName(QStringLiteral("org.qtmonitor.app"));

    // Two catalogs, both installed before any widget is built so no string is
    // constructed in the wrong language. Qt's own comes first so that ours
    // wins on the rare key they share.
    //
    // qtbase carries the text Qt draws itself — the buttons in QMessageBox and
    // QDialogButtonBox are Qt's strings, not ours, and stay English without
    // it. Both loads are allowed to fail: a locale with no catalog is the
    // normal case, and English is the correct result.
    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QCoreApplication::installTranslator(&qtTranslator);
    }

    QTranslator appTranslator;
    if (appTranslator.load(QLocale(), QStringLiteral("qtmonitor"),
                           QStringLiteral("_"), QStringLiteral(":/i18n"))) {
        QCoreApplication::installTranslator(&appTranslator);
    }

    // A light/dark override, if the user set one, has to be in place before the
    // first widget is built or the window paints twice on startup.
    Settings::instance().applyTheme();

    // Respect the DE's active icon theme; fall back to our bundled icon so the
    // app never renders with a missing icon on minimal themes.
    const QIcon fallback(QStringLiteral(":/icons/qtmonitor.svg"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("qtmonitor"), fallback));

    MainWindow window;
    window.show();

    return app.exec();
}
