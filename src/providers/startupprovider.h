#pragma once

#include <QString>
#include <QVector>

// One thing that runs when the user logs in.
struct StartupEntry {
    enum class Kind {
        XdgAutostart, // a .desktop file in an autostart directory
        SystemdUser,  // a systemd --user unit with an [Install] section
    };

    enum class Status {
        Enabled,      // will run at the next login
        Disabled,     // present but switched off
        OtherDesktop, // restricted by OnlyShowIn/NotShowIn to a desktop that
                      // is not the one running now, so it will not launch here
        Missing,      // TryExec names a binary that is not installed
    };

    Kind kind = Kind::XdgAutostart;
    Status status = Status::Enabled;

    QString id;          // .desktop basename, or the unit name
    QString name;        // localized Name=, or the unit Description
    QString description; // Comment=, or the unit Id
    QString command;     // Exec=, for the detail column
    QString sourcePath;  // the file this came from
    // False for a system-wide entry with no user override yet. Toggling one
    // still works — it writes a user-level override — but the distinction is
    // worth showing.
    bool userOwned = false;
    // Desktops this entry is restricted to, when status is OtherDesktop.
    QString desktopRestriction;

    bool isEnabled() const { return status == Status::Enabled; }
    // Only Enabled/Disabled are meaningful to flip. A unit with no [Install]
    // section, or an entry for another desktop, is informational.
    bool toggleable() const
    {
        return status == Status::Enabled || status == Status::Disabled;
    }
};

// Enumerates and toggles login-time startup items.
//
// Two sources, deliberately kept distinct in the UI because they are edited
// in different places:
//
//   * XDG autostart .desktop files, from ~/.config/autostart (user) layered
//     over the directories in $XDG_CONFIG_DIRS (system-wide). A user file
//     shadows a system file of the same name, which is also how disabling a
//     system entry works.
//   * systemd --user service units that declare an [Install] section, so
//     they can actually be enabled or disabled.
//
// On a systemd session the XDG entries ALSO appear as generated
// app-<name>@autostart.service units. Those are filtered out rather than
// listed twice: the .desktop file is the thing a user edits, and the
// generated unit just mirrors it.
class StartupProvider
{
public:
    bool isAvailable() const;

    QVector<StartupEntry> scan() const;

    // Returns an empty string on success, or a human-readable reason. This is
    // the only part of the app that writes state outside its own config.
    QString setEnabled(const StartupEntry &entry, bool enabled) const;

    // Desktops named in XDG_CURRENT_DESKTOP, used for OnlyShowIn/NotShowIn.
    static QStringList currentDesktops();

private:
    QVector<StartupEntry> scanXdgAutostart() const;
    QVector<StartupEntry> scanSystemdUser() const;

    QString setXdgEnabled(const StartupEntry &entry, bool enabled) const;
    QString setSystemdEnabled(const StartupEntry &entry, bool enabled) const;
};
