#pragma once

#include <QObject>
#include <QVariantMap>

#include <KAuth/ActionReply>

// Privileged helper, executed as root by the KAuth/polkit D-Bus backend.
//
// This binary runs with full privileges, so it is kept deliberately tiny and
// shares no code with the application: it validates its arguments and calls
// kill(2), nothing else. Every argument arrives from an unprivileged process
// and is therefore treated as untrusted.
class QtmonitorHelper : public QObject
{
    Q_OBJECT

public Q_SLOTS:
    // Slot name must match the last component of org.qtmonitor.signalprocess.
    // Expected arguments:
    //   pid        (int)         target process
    //   signal     (int)         0 = SIGTERM, 1 = SIGKILL; no other value is
    //                            accepted, so a raw signal number can never
    //                            be smuggled through
    //   startTicks (qulonglong)  /proc/PID/stat field 22, checked against the
    //                            live process to defeat PID reuse
    KAuth::ActionReply signalprocess(const QVariantMap &args);
};
