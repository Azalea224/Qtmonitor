#include "processcontrol.h"

#include <QFile>
#include <QVariant>

#include <KAuth/Action>
#include <KAuth/ActionReply>
#include <KAuth/ExecuteJob>
#include <KJob>

#include <cerrno>
#include <csignal>

namespace processcontrol {

Result send(int pid, Signal signal)
{
    if (pid <= 0) {
        return Result::Failed;
    }

    const int sig = signal == Signal::Kill ? SIGKILL : SIGTERM;
    if (::kill(static_cast<pid_t>(pid), sig) == 0) {
        return Result::Ok;
    }

    switch (errno) {
    case EPERM:
        return Result::NotPermitted;
    case ESRCH:
        return Result::NoSuchProcess;
    default:
        return Result::Failed;
    }
}

bool exists(int pid)
{
    if (pid <= 0) {
        return false;
    }
    // Signal 0 performs permission and existence checks without delivering
    // anything, so EPERM still means the process is alive.
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

bool isZombie(int pid)
{
    QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray stat = file.readAll();
    // The state field follows the last ')', since comm may contain parens.
    const int close = stat.lastIndexOf(')');
    if (close < 0 || close + 2 >= stat.size()) {
        return false;
    }
    return stat.at(close + 2) == 'Z';
}

QString signalName(Signal signal)
{
    return signal == Signal::Kill ? QStringLiteral("SIGKILL")
                                  : QStringLiteral("SIGTERM");
}

bool privilegedBackendAvailable()
{
    // Checked as files rather than by asking polkit: running from the build
    // directory is the normal development case, and it must produce a clear
    // "not installed" message instead of a confusing authorization failure.
#if defined(QTMONITOR_KAUTH_HELPER_PATH) && defined(QTMONITOR_KAUTH_POLICY_PATH)
    return QFile::exists(QStringLiteral(QTMONITOR_KAUTH_HELPER_PATH))
        && QFile::exists(QStringLiteral(QTMONITOR_KAUTH_POLICY_PATH));
#else
    return false;
#endif
}

void sendPrivileged(int pid, Signal signal, quint64 startTicks,
                    QObject *context,
                    std::function<void(const PrivilegedOutcome &)> done)
{
    if (!privilegedBackendAvailable()) {
        PrivilegedOutcome outcome;
        outcome.message =
            QStringLiteral("Qtmonitor is not installed system-wide, so it "
                           "cannot ask for authorization. The privileged "
                           "helper and its polkit policy are only present "
                           "after installing the package.");
        done(outcome);
        return;
    }

    KAuth::Action action(QStringLiteral("org.qtmonitor.signalprocess"));
    action.setHelperId(QStringLiteral("org.qtmonitor"));
    action.addArgument(QStringLiteral("pid"), pid);
    // An enum, not a signal number: the helper refuses anything else.
    action.addArgument(QStringLiteral("signal"),
                       signal == Signal::Kill ? 1 : 0);
    action.addArgument(QStringLiteral("startTicks"),
                       QVariant::fromValue<qulonglong>(startTicks));

    if (!action.isValid()) {
        PrivilegedOutcome outcome;
        outcome.message = QStringLiteral("The authorization action is not "
                                         "registered on this system.");
        done(outcome);
        return;
    }

    KAuth::ExecuteJob *job = action.execute();
    QObject::connect(job, &KJob::result, context ? context : job,
                     [done, job] {
                         PrivilegedOutcome outcome;
                         switch (job->error()) {
                         case 0:
                             outcome.authorized = true;
                             outcome.succeeded = true;
                             break;
                         case KAuth::ActionReply::UserCancelledError:
                         case KAuth::ActionReply::AuthorizationDeniedError:
                             // Left unauthorized: the user declined or failed
                             // to authenticate, which is not an error to
                             // report as a failure.
                             break;
                         default:
                             // Authentication succeeded but the helper
                             // refused or could not act; its description
                             // explains why.
                             outcome.authorized = true;
                             outcome.message = job->errorString();
                             break;
                         }
                         done(outcome);
                     });
    job->start();
}

QString describe(Result result)
{
    switch (result) {
    case Result::Ok:
        return QStringLiteral("Signal sent.");
    case Result::NotPermitted:
        return QStringLiteral("Not permitted — this process belongs to "
                              "another user.");
    case Result::NoSuchProcess:
        return QStringLiteral("The process has already exited.");
    case Result::Failed:
        break;
    }
    return QStringLiteral("Could not signal the process.");
}

} // namespace processcontrol
