#pragma once

#include <QString>

#include <functional>

class QObject;

// Sending signals to processes. Phase 3a covers processes we own; the
// NotPermitted result is the hook Phase 3b escalates through KAuth/polkit.
namespace processcontrol {

enum class Result {
    Ok,
    NotPermitted, // EPERM: another user's or a root process
    NoSuchProcess, // ESRCH: already exited
    Failed,
};

// Linux signal names are used verbatim in the UI rather than being hidden
// behind "End task": knowing whether it was SIGTERM or SIGKILL matters.
enum class Signal {
    Term, // SIGTERM, polite: lets the process clean up
    Kill, // SIGKILL, unblockable
};

Result send(int pid, Signal signal);

// Whether the process still exists, used to decide if a SIGTERM landed.
// Note that a zombie still "exists": it occupies a PID until its parent
// reaps it.
bool exists(int pid);

// A zombie has already exited and cannot be signalled away — only the
// parent reaping it (or the parent dying) clears it. Worth distinguishing,
// because offering SIGKILL for one is a dead end.
bool isZombie(int pid);

QString describe(Result result);
QString signalName(Signal signal);

// Escalated send, for processes this session does not own. Routed through a
// KAuth helper so polkit presents its standard authentication dialog.
//
// Asynchronous on purpose: the prompt stays up for as long as the user takes
// to authenticate, and blocking the GUI thread on a nested event loop for
// that is a re-entrancy trap. `context` scopes the callback — if it is
// destroyed first, the callback is dropped.
//
// startTicks comes from ProcessInfo::startTicks and lets the helper confirm
// the PID still refers to the same process before signalling it.
//
// `denied` distinguishes the user cancelling or failing authentication from
// the action failing after a successful authentication.
struct PrivilegedOutcome {
    bool authorized = false; // false if polkit denied or the user cancelled
    bool succeeded = false;
    QString message;
};

void sendPrivileged(int pid, Signal signal, quint64 startTicks,
                    QObject *context,
                    std::function<void(const PrivilegedOutcome &)> done);

// Whether a privileged backend is usable at all. False means the helper or
// its polkit policy is not installed, e.g. running straight from the build
// directory, and the UI should say so rather than appearing to hang.
bool privilegedBackendAvailable();

} // namespace processcontrol
