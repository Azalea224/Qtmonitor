#include "sessionprovider.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QVariantMap>

namespace {

constexpr char kLogindService[] = "org.freedesktop.login1";
constexpr char kLogindPath[] = "/org/freedesktop/login1";
constexpr char kManagerInterface[] = "org.freedesktop.login1.Manager";
constexpr char kSessionInterface[] = "org.freedesktop.login1.Session";
constexpr char kPropertiesInterface[] = "org.freedesktop.DBus.Properties";

constexpr int kTimeoutMsec = 2000;

// One entry of ListSessions' a(susso) return.
struct SessionRecord {
    QString id;
    uint uid = 0;
    QString user;
    QString seat;
    QDBusObjectPath path;
};

} // namespace

SessionProvider::SessionProvider()
{
    // logind lives on the system bus. Its absence is a normal state, not an
    // error: the Users tab simply loses its session columns.
    m_available = QDBusConnection::systemBus().isConnected();
}

QHash<uint, QVector<UserSession>> SessionProvider::sessionsByUid() const
{
    QHash<uint, QVector<UserSession>> result;
    if (!m_available) {
        return result;
    }

    QDBusConnection bus = QDBusConnection::systemBus();

    QDBusMessage list = QDBusMessage::createMethodCall(
        QLatin1String(kLogindService), QLatin1String(kLogindPath),
        QLatin1String(kManagerInterface), QStringLiteral("ListSessions"));
    const QDBusMessage listReply = bus.call(list, QDBus::Block, kTimeoutMsec);
    if (listReply.type() != QDBusMessage::ReplyMessage || listReply.arguments().isEmpty()) {
        return result;
    }

    // ListSessions returns a(susso). Demarshalled by hand rather than through
    // a registered metatype: the struct is local to this file and only ever
    // read, so a custom type registration would buy nothing.
    QVector<SessionRecord> records;
    const QDBusArgument argument = listReply.arguments().first().value<QDBusArgument>();
    argument.beginArray();
    while (!argument.atEnd()) {
        SessionRecord record;
        argument.beginStructure();
        argument >> record.id >> record.uid >> record.user >> record.seat >> record.path;
        argument.endStructure();
        records.append(record);
    }
    argument.endArray();

    for (const SessionRecord &record : records) {
        UserSession session;
        session.id = record.id;
        session.seat = record.seat;

        // One GetAll per session rather than a Get per property: sessions are
        // few, and this keeps it to a single round trip each.
        QDBusMessage properties = QDBusMessage::createMethodCall(
            QLatin1String(kLogindService), record.path.path(),
            QLatin1String(kPropertiesInterface), QStringLiteral("GetAll"));
        properties << QLatin1String(kSessionInterface);

        const QDBusMessage reply = bus.call(properties, QDBus::Block, kTimeoutMsec);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            QVariantMap map;
            const QDBusArgument mapArgument =
                reply.arguments().first().value<QDBusArgument>();
            mapArgument >> map;

            session.type = map.value(QStringLiteral("Type")).toString();
            session.state = map.value(QStringLiteral("State")).toString();
            session.desktop = map.value(QStringLiteral("Desktop")).toString();
            session.active = map.value(QStringLiteral("Active")).toBool();
            session.remote = map.value(QStringLiteral("Remote")).toBool();
            session.remoteHost = map.value(QStringLiteral("RemoteHost")).toString();
            if (session.seat.isEmpty()) {
                session.seat = map.value(QStringLiteral("Seat")).toString();
            }
        }
        result[record.uid].append(session);
    }
    return result;
}

QString SessionProvider::describe(const QVector<UserSession> &sessions)
{
    if (sessions.isEmpty()) {
        // A user with processes but no session: a system account running
        // daemons, which is normal and worth naming rather than blanking.
        return QCoreApplication::translate("SessionProvider",
                                           "No login session");
    }

    // The graphical or active session is the one worth summarising; a user
    // typically also owns a "manager" session that says nothing useful.
    const UserSession *best = &sessions.first();
    for (const UserSession &session : sessions) {
        if (session.active && !session.type.isEmpty()
            && session.type != QStringLiteral("unspecified")) {
            best = &session;
            break;
        }
    }

    QStringList parts;
    if (!best->type.isEmpty()) {
        parts << best->type;
    }
    if (!best->desktop.isEmpty()) {
        parts << best->desktop;
    }
    if (!best->seat.isEmpty()) {
        parts << best->seat;
    }
    if (best->remote && !best->remoteHost.isEmpty()) {
        parts << QCoreApplication::translate("SessionProvider", "from %1")
                     .arg(best->remoteHost);
    }
    if (!best->state.isEmpty()) {
        parts << best->state;
    }
    if (sessions.size() > 1) {
        parts << QCoreApplication::translate("SessionProvider", "%n session(s)",
                                             nullptr, sessions.size());
    }
    return parts.join(QStringLiteral(" · "));
}
