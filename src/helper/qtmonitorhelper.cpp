#include "qtmonitorhelper.h"

#include <QFile>

#include <cerrno>
#include <csignal>
#include <cstring>

namespace {

// Rejecting the whole request is always safe here, so every failure path
// returns an error rather than guessing.
KAuth::ActionReply refuse(const QString &why)
{
    KAuth::ActionReply reply = KAuth::ActionReply::HelperErrorReply();
    reply.setErrorDescription(why);
    return reply;
}

// Reads field 22 (starttime) of /proc/PID/stat. Parsing starts after the
// last ')' because comm may contain spaces and parentheses.
bool readStartTicks(int pid, quint64 &startTicksOut)
{
    QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray stat = file.readAll();
    const int close = stat.lastIndexOf(')');
    if (close < 0) {
        return false;
    }
    const QList<QByteArray> fields = stat.mid(close + 2).simplified().split(' ');
    constexpr int kStartTimeIndex = 19; // field 22, counting from state = 0
    if (fields.size() <= kStartTimeIndex) {
        return false;
    }
    bool ok = false;
    startTicksOut = fields.at(kStartTimeIndex).toULongLong(&ok);
    return ok;
}

} // namespace

KAuth::ActionReply QtmonitorHelper::signalprocess(const QVariantMap &args)
{
    bool pidOk = false;
    const int pid = args.value(QStringLiteral("pid")).toInt(&pidOk);
    if (!pidOk) {
        return refuse(QStringLiteral("Malformed request: no target process."));
    }
    // pid 1 is init/systemd: signalling it would take the system down, and no
    // task manager has a legitimate reason to. Anything below it is invalid,
    // and negative pids would address entire process groups.
    if (pid <= 1) {
        return refuse(QStringLiteral("Refusing to signal PID %1 — process IDs "
                                     "0 and 1 and process groups are not valid "
                                     "targets.")
                          .arg(pid));
    }

    // Map an enum, never a caller-supplied signal number.
    int sig = 0;
    switch (args.value(QStringLiteral("signal")).toInt()) {
    case 0:
        sig = SIGTERM;
        break;
    case 1:
        sig = SIGKILL;
        break;
    default:
        return refuse(QStringLiteral("Refusing to send an unsupported signal; "
                                     "only SIGTERM and SIGKILL are allowed."));
    }

    bool ticksOk = false;
    const quint64 expectedTicks =
        args.value(QStringLiteral("startTicks")).toULongLong(&ticksOk);
    if (!ticksOk) {
        return refuse(QStringLiteral("Malformed request: missing process "
                                     "start time."));
    }

    // PID reuse guard. Between the user clicking and this helper running, the
    // target could have exited and its PID been recycled onto something else
    // — possibly something critical. The start time makes the identity exact.
    quint64 actualTicks = 0;
    if (!readStartTicks(pid, actualTicks)) {
        return refuse(QStringLiteral("Process %1 no longer exists.").arg(pid));
    }
    if (actualTicks != expectedTicks) {
        return refuse(QStringLiteral("Process %1 is not the process that was "
                                     "selected — the PID has been reused. "
                                     "Nothing was signalled.")
                          .arg(pid));
    }

    if (::kill(static_cast<pid_t>(pid), sig) != 0) {
        return refuse(QStringLiteral("Could not signal process %1: %2.")
                          .arg(pid)
                          .arg(QString::fromLocal8Bit(strerror(errno))));
    }

    return KAuth::ActionReply::SuccessReply();
}
