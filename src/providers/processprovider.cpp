#include "processprovider.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <iterator>

namespace {

// Reads a whole /proc file. Deliberately uses readAll() rather than
// QTextStream line loops: procfs reports st_size 0, which makes atEnd() true
// immediately, but readAll() keeps reading until EOF and works correctly.
QByteArray readProcFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

quint64 totalCpuTicksFromStat()
{
    const QByteArray data = readProcFile(QStringLiteral("/proc/stat"));
    if (data.isEmpty()) {
        return 0;
    }
    // First line is the "cpu" aggregate; summing its fields gives total
    // capacity across every core, which is what per-process shares divide by.
    const QByteArray firstLine = data.left(data.indexOf('\n'));
    const QList<QByteArray> parts = firstLine.simplified().split(' ');
    quint64 total = 0;
    // Skip the "cpu" label. guest/guest_nice are already counted inside
    // user/nice, so stop at the first 8 numeric fields.
    for (int i = 1; i < parts.size() && i <= 8; ++i) {
        total += parts.at(i).toULongLong();
    }
    return total;
}

long pageSizeBytes()
{
    static const long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? size : 4096;
}

} // namespace

bool ProcfsProcessProvider::isAvailable() const
{
    return QFile::exists(QStringLiteral("/proc/self/stat"));
}

QString ProcfsProcessProvider::userNameFor(uint uid)
{
    const auto cached = m_userNames.constFind(uid);
    if (cached != m_userNames.cend()) {
        return *cached;
    }
    // getpwuid hits nsswitch (possibly LDAP/SSSD), so this must stay cached.
    QString name;
    if (const passwd *pw = getpwuid(static_cast<uid_t>(uid))) {
        name = QString::fromLocal8Bit(pw->pw_name);
    } else {
        name = QString::number(uid);
    }
    m_userNames.insert(uid, name);
    return name;
}

ProcfsProcessProvider::CachedStatics
ProcfsProcessProvider::staticsFor(int pid, quint64 startTicks)
{
    const auto existing = m_statics.constFind(pid);
    if (existing != m_statics.cend() && existing->startTicks == startTicks) {
        return *existing;
    }

    const QString procDir = QStringLiteral("/proc/%1").arg(pid);

    CachedStatics statics;
    statics.startTicks = startTicks;

    // cmdline is NUL-separated; empty means a kernel thread.
    QByteArray cmdline = readProcFile(procDir + QStringLiteral("/cmdline"));
    while (cmdline.endsWith('\0')) {
        cmdline.chop(1);
    }
    statics.isKernelThread = cmdline.isEmpty();
    cmdline.replace('\0', ' ');
    statics.command = QString::fromLocal8Bit(cmdline);

    // Ownership of /proc/PID is the process owner: one stat call, versus
    // parsing /proc/PID/status for the same answer.
    const QFileInfo info(procDir);
    statics.uid = info.ownerId();
    statics.user = userNameFor(statics.uid);

    m_statics.insert(pid, statics);
    return statics;
}

ProcessSnapshot ProcfsProcessProvider::sample()
{
    ProcessSnapshot snapshot;

    const quint64 totalTicks = totalCpuTicksFromStat();
    const quint64 totalDelta = m_havePrevious && totalTicks > m_previousTotalTicks
        ? totalTicks - m_previousTotalTicks
        : 0;

    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsedMsec = m_havePrevious ? nowMsec - m_previousSampleMsec : 0;

    const QStringList pidDirs =
        QDir(QStringLiteral("/proc")).entryList(QStringList{QStringLiteral("[0-9]*")},
                                                QDir::Dirs | QDir::NoDotAndDotDot);

    QHash<int, PrevCounters> current;
    current.reserve(pidDirs.size());
    snapshot.processes.reserve(pidDirs.size());

    for (const QString &entry : pidDirs) {
        bool pidOk = false;
        const int pid = entry.toInt(&pidOk);
        if (!pidOk) {
            continue;
        }

        const QString procDir = QStringLiteral("/proc/") + entry;
        const QByteArray stat = readProcFile(procDir + QStringLiteral("/stat"));
        if (stat.isEmpty()) {
            continue; // process exited between listing and reading
        }

        // comm may itself contain spaces and parentheses ("(Web Content)"),
        // so the field split must start after the LAST ')'.
        const int nameOpen = stat.indexOf('(');
        const int nameClose = stat.lastIndexOf(')');
        if (nameOpen < 0 || nameClose < nameOpen) {
            continue;
        }

        ProcessInfo info;
        info.pid = pid;
        info.name = QString::fromLocal8Bit(
            stat.mid(nameOpen + 1, nameClose - nameOpen - 1));

        // Fields after comm, so index i here is proc(5) field (i + 3).
        const QList<QByteArray> f =
            stat.mid(nameClose + 2).simplified().split(' ');
        constexpr int kRssIndex = 21; // field 24, the highest one we read
        if (f.size() <= kRssIndex) {
            continue;
        }

        info.state = QChar::fromLatin1(f.at(0).isEmpty() ? '?' : f.at(0).at(0));
        info.ppid = f.at(1).toInt();
        const quint64 utime = f.at(11).toULongLong();
        const quint64 stime = f.at(12).toULongLong();
        info.nice = f.at(16).toInt();
        info.threads = f.at(17).toInt();
        const quint64 startTicks = f.at(19).toULongLong();
        info.startTicks = startTicks;
        info.rssBytes = f.at(kRssIndex).toULongLong()
            * static_cast<quint64>(pageSizeBytes());

        const CachedStatics statics = staticsFor(pid, startTicks);
        info.command = statics.command;
        info.user = statics.user;
        info.uid = statics.uid;
        info.isKernelThread = statics.isKernelThread;
        if (info.isKernelThread) {
            ++snapshot.kernelThreadCount;
        }

        PrevCounters counters;
        counters.cpuTicks = utime + stime;
        counters.startTicks = startTicks;

        // /proc/PID/io is 0400 root for other users' processes; an empty read
        // means "not permitted", not "no I/O".
        const QByteArray io = readProcFile(procDir + QStringLiteral("/io"));
        if (!io.isEmpty()) {
            info.ioAccessible = true;
            for (const QByteArray &line : io.split('\n')) {
                if (line.startsWith("read_bytes:")) {
                    counters.readBytes = line.mid(11).simplified().toULongLong();
                } else if (line.startsWith("write_bytes:")) {
                    counters.writeBytes = line.mid(12).simplified().toULongLong();
                }
            }
        }

        const auto previous = m_previous.constFind(pid);
        const bool comparable = previous != m_previous.cend()
            && previous->startTicks == startTicks;

        if (comparable && totalDelta > 0 && counters.cpuTicks >= previous->cpuTicks) {
            info.cpuPercent = 100.0 * (counters.cpuTicks - previous->cpuTicks)
                / static_cast<double>(totalDelta);
        }
        if (comparable && info.ioAccessible && elapsedMsec > 0) {
            const auto rate = [elapsedMsec](quint64 now, quint64 before) -> quint64 {
                if (now <= before) {
                    return 0;
                }
                return (now - before) * 1000 / static_cast<quint64>(elapsedMsec);
            };
            info.diskReadBytesPerSec = rate(counters.readBytes, previous->readBytes);
            info.diskWriteBytesPerSec = rate(counters.writeBytes, previous->writeBytes);
        }

        current.insert(pid, counters);
        snapshot.processes.append(info);
    }

    // Drop cached statics for processes that are gone, so the caches do not
    // grow without bound on a long-running session.
    for (auto it = m_statics.begin(); it != m_statics.end();) {
        it = current.contains(it.key()) ? std::next(it) : m_statics.erase(it);
    }

    m_previous = std::move(current);
    m_previousTotalTicks = totalTicks;
    m_previousSampleMsec = nowMsec;
    m_havePrevious = true;

    std::sort(snapshot.processes.begin(), snapshot.processes.end(),
              [](const ProcessInfo &a, const ProcessInfo &b) { return a.pid < b.pid; });
    return snapshot;
}
