#pragma once

#include <QHash>
#include <QString>
#include <QVector>

// One logind session belonging to a user.
struct UserSession {
    QString id;      // logind session id, e.g. "2"
    QString type;    // "wayland", "x11", "tty", ...
    QString seat;    // "seat0", empty for a non-seated session
    QString state;   // "active", "online", "closing"
    QString desktop; // XDG_CURRENT_DESKTOP as logind recorded it
    QString remoteHost;
    bool remote = false;
    bool active = false;
};

// Login sessions, read from systemd-logind over D-Bus.
//
// Deliberately NOT parsed out of /run/systemd/sessions: those files open with
// "# This is private data. Do not parse." and their format carries no
// stability promise. The D-Bus interface is the supported one.
//
// This is enrichment only. The Users tab is built from /proc, which always
// works; logind merely explains *how* a user is logged in. A machine without
// systemd, or with the service unreachable, loses the session columns and
// nothing else.
class SessionProvider
{
public:
    SessionProvider();

    bool isAvailable() const { return m_available; }

    // Sessions grouped by owning uid. Empty when logind is unreachable.
    QHash<uint, QVector<UserSession>> sessionsByUid() const;

    // Human-readable summary for a user row: "wayland · seat0 · active".
    static QString describe(const QVector<UserSession> &sessions);

private:
    bool m_available = false;
};
