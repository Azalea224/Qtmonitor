#include "startupprovider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <KConfigGroup>
#include <KDesktopFile>

#include <algorithm>

#include "optionaltools.h"

namespace {

constexpr int kSystemctlTimeoutMsec = 5000;

// Autostart directories, most-significant first: the user's own directory
// shadows the system ones, which is how the spec expresses an override.
QString userAutostartDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + QStringLiteral("/autostart");
}

QStringList systemAutostartDirs()
{
    QStringList dirs;
    const QByteArray configDirs = qgetenv("XDG_CONFIG_DIRS");
    const QString value = configDirs.isEmpty() ? QStringLiteral("/etc/xdg")
                                               : QString::fromLocal8Bit(configDirs);
    const QStringList parts = value.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        dirs.append(part + QStringLiteral("/autostart"));
    }
    return dirs;
}

// Runs systemctl --user and returns its stdout, or a null QString on failure.
QString runSystemctl(const QStringList &arguments, int *exitCodeOut = nullptr,
                     QString *stderrOut = nullptr)
{
    QProcess process;
    process.start(optionaltools::path(optionaltools::Tool::Systemctl),
                  QStringList{QStringLiteral("--user")} + arguments);
    if (!process.waitForFinished(kSystemctlTimeoutMsec)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    if (exitCodeOut) {
        *exitCodeOut = process.exitCode();
    }
    if (stderrOut) {
        *stderrOut = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    }
    return QString::fromLocal8Bit(process.readAllStandardOutput());
}

} // namespace

QStringList StartupProvider::currentDesktops()
{
    // XDG_CURRENT_DESKTOP is colon-separated and ordered most-specific-first
    // ("wlroots:Hyprland"). Matching any component is enough.
    const QByteArray raw = qgetenv("XDG_CURRENT_DESKTOP");
    if (raw.isEmpty()) {
        return {};
    }
    return QString::fromLocal8Bit(raw).split(QLatin1Char(':'), Qt::SkipEmptyParts);
}

bool StartupProvider::isAvailable() const
{
    // The XDG half needs nothing at all, so this source is always usable.
    return true;
}

QVector<StartupEntry> StartupProvider::scanXdgAutostart() const
{
    // Keyed by .desktop basename so a user file shadows the system one.
    QVector<StartupEntry> entries;
    QHash<QString, int> indexByFile;

    struct Source {
        QString dir;
        bool userOwned;
    };
    QVector<Source> sources;
    sources.append({userAutostartDir(), true});
    const QStringList systemDirs = systemAutostartDirs();
    for (const QString &dir : systemDirs) {
        sources.append({dir, false});
    }

    const QStringList desktops = currentDesktops();

    for (const Source &source : sources) {
        const QDir dir(source.dir);
        if (!dir.exists()) {
            continue;
        }
        const QStringList files =
            dir.entryList(QStringList{QStringLiteral("*.desktop")}, QDir::Files);

        for (const QString &file : files) {
            if (indexByFile.contains(file)) {
                continue; // already provided by a higher-priority directory
            }

            const QString path = dir.filePath(file);
            KDesktopFile desktop(path);
            const KConfigGroup group = desktop.desktopGroup();

            StartupEntry entry;
            entry.kind = StartupEntry::Kind::XdgAutostart;
            entry.id = file;
            entry.sourcePath = path;
            entry.userOwned = source.userOwned;
            entry.name = desktop.readName();
            if (entry.name.isEmpty()) {
                entry.name = QFileInfo(file).completeBaseName();
            }
            entry.description = desktop.readComment();
            entry.command = group.readEntry("Exec", QString());

            // Two disable conventions in the wild. Hidden= is the one the
            // spec defines; X-GNOME-Autostart-enabled= is what several
            // desktop tools write, and systemd's generator honours both.
            const bool hidden = group.readEntry("Hidden", false);
            const bool gnomeEnabled =
                group.readEntry("X-GNOME-Autostart-enabled", true);

            const QStringList onlyShowIn =
                group.readEntry("OnlyShowIn", QString())
                    .split(QLatin1Char(';'), Qt::SkipEmptyParts);
            const QStringList notShowIn =
                group.readEntry("NotShowIn", QString())
                    .split(QLatin1Char(';'), Qt::SkipEmptyParts);

            const auto matchesHere = [&desktops](const QStringList &list) {
                return std::any_of(list.cbegin(), list.cend(),
                                   [&desktops](const QString &name) {
                                       return desktops.contains(name, Qt::CaseInsensitive);
                                   });
            };

            if (hidden || !gnomeEnabled) {
                entry.status = StartupEntry::Status::Disabled;
            } else if (!onlyShowIn.isEmpty() && !matchesHere(onlyShowIn)) {
                // Deliberately listed rather than hidden: on a desktop that
                // is not Plasma or GNOME, seeing that half these entries will
                // never run is the useful information.
                entry.status = StartupEntry::Status::OtherDesktop;
                entry.desktopRestriction = onlyShowIn.join(QStringLiteral(", "));
            } else if (!notShowIn.isEmpty() && matchesHere(notShowIn)) {
                entry.status = StartupEntry::Status::OtherDesktop;
                entry.desktopRestriction =
                    tr("all except %1").arg(notShowIn.join(QStringLiteral(", ")));
            } else if (!desktop.tryExec()) {
                entry.status = StartupEntry::Status::Missing;
            } else {
                entry.status = StartupEntry::Status::Enabled;
            }

            indexByFile.insert(file, entries.size());
            entries.append(entry);
        }
    }
    return entries;
}

QVector<StartupEntry> StartupProvider::scanSystemdUser() const
{
    if (!optionaltools::isAvailable(optionaltools::Tool::Systemctl)) {
        return {};
    }

    const QString listing = runSystemctl({QStringLiteral("list-unit-files"),
                                          QStringLiteral("--type=service"),
                                          QStringLiteral("--no-pager"),
                                          QStringLiteral("--no-legend")});
    if (listing.isNull()) {
        return {};
    }

    QVector<StartupEntry> entries;
    QStringList unitNames;

    const QStringList lines = listing.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields =
            line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() < 2) {
            continue;
        }
        const QString unit = fields.at(0);
        const QString state = fields.at(1);

        // Only units carrying an [Install] section can be switched. static,
        // alias, transient, indirect and masked units are noise here, and
        // "generated" is where the XDG entries reappear as
        // app-<name>@autostart.service — listing those would duplicate the
        // .desktop files the user actually edits.
        if (state != QStringLiteral("enabled") && state != QStringLiteral("disabled")) {
            continue;
        }

        // "foo@.service" is a template, not a unit: it cannot be enabled
        // without an instance name, so offering a switch would only ever
        // produce an error. Instantiated units ("foo@bar.service") are real
        // and stay in the list.
        if (unit.endsWith(QStringLiteral("@.service"))) {
            continue;
        }

        StartupEntry entry;
        entry.kind = StartupEntry::Kind::SystemdUser;
        entry.id = unit;
        entry.name = unit;
        entry.userOwned = true;
        entry.status = state == QStringLiteral("enabled")
            ? StartupEntry::Status::Enabled
            : StartupEntry::Status::Disabled;
        entries.append(entry);
        unitNames.append(unit);
    }

    if (unitNames.isEmpty()) {
        return entries;
    }

    // One batched call for the descriptions; querying each unit separately
    // would be 30-odd process spawns.
    const QString details =
        runSystemctl(QStringList{QStringLiteral("show")} + unitNames
                     + QStringList{QStringLiteral("--property=Id,Description,FragmentPath"),
                                   QStringLiteral("--no-pager")});

    // Records are separated by a blank line, in the order asked for.
    const QStringList records = details.split(QStringLiteral("\n\n"), Qt::SkipEmptyParts);
    QHash<QString, QPair<QString, QString>> byUnit; // id -> (description, path)
    for (const QString &record : records) {
        QString id;
        QString description;
        QString fragment;
        const QStringList recordLines =
            record.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : recordLines) {
            const int equals = line.indexOf(QLatin1Char('='));
            if (equals < 0) {
                continue;
            }
            const QString key = line.left(equals);
            const QString value = line.mid(equals + 1);
            if (key == QStringLiteral("Id")) {
                id = value;
            } else if (key == QStringLiteral("Description")) {
                description = value;
            } else if (key == QStringLiteral("FragmentPath")) {
                fragment = value;
            }
        }
        if (!id.isEmpty()) {
            byUnit.insert(id, {description, fragment});
        }
    }

    for (StartupEntry &entry : entries) {
        const auto found = byUnit.constFind(entry.id);
        if (found == byUnit.cend()) {
            continue;
        }
        if (!found->first.isEmpty()) {
            entry.name = found->first;
            entry.description = entry.id;
        }
        entry.sourcePath = found->second;
    }
    return entries;
}

QVector<StartupEntry> StartupProvider::scan() const
{
    QVector<StartupEntry> entries = scanXdgAutostart();
    entries += scanSystemdUser();

    std::sort(entries.begin(), entries.end(),
              [](const StartupEntry &a, const StartupEntry &b) {
                  if (a.kind != b.kind) {
                      return a.kind < b.kind;
                  }
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });
    return entries;
}

QString StartupProvider::setXdgEnabled(const StartupEntry &entry, bool enabled) const
{
    const QString userDir = userAutostartDir();
    const QString userPath = userDir + QLatin1Char('/') + entry.id;

    if (!QDir().mkpath(userDir)) {
        return tr("Could not create %1.").arg(userDir);
    }

    // A system-wide entry is never edited in place — that would need root and
    // would be undone by the next package update. The spec's mechanism is a
    // user-level file of the same name, which shadows it entirely.
    if (!QFile::exists(userPath)) {
        if (!QFile::copy(entry.sourcePath, userPath)) {
            return tr("Could not copy %1 to %2.")
                .arg(entry.sourcePath, userPath);
        }
        // QFile::copy preserves the source's read-only permissions.
        QFile::setPermissions(userPath,
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup
                                  | QFile::ReadOther);
    }

    KDesktopFile desktop(userPath);
    KConfigGroup group = desktop.desktopGroup();
    group.writeEntry("Hidden", !enabled);
    // Rewritten too, so an entry disabled by the other convention comes back.
    if (group.hasKey("X-GNOME-Autostart-enabled") || enabled) {
        group.writeEntry("X-GNOME-Autostart-enabled", enabled);
    }
    group.sync();

    if (!group.config()->isConfigWritable(true)) {
        return tr("%1 is not writable.").arg(userPath);
    }
    return {};
}

QString StartupProvider::setSystemdEnabled(const StartupEntry &entry, bool enabled) const
{
    if (!optionaltools::isAvailable(optionaltools::Tool::Systemctl)) {
        return tr("systemctl is not installed.");
    }

    int exitCode = -1;
    QString errorOutput;
    // --user only: these units belong to this session's own manager, so no
    // polkit prompt is involved and nothing escalates.
    runSystemctl({enabled ? QStringLiteral("enable") : QStringLiteral("disable"),
                  entry.id},
                 &exitCode, &errorOutput);

    if (exitCode != 0) {
        return errorOutput.isEmpty()
            ? tr("systemctl %1 %2 failed.")
                  .arg(enabled ? QStringLiteral("enable") : QStringLiteral("disable"),
                       entry.id)
            : errorOutput;
    }
    return {};
}

QString StartupProvider::setEnabled(const StartupEntry &entry, bool enabled) const
{
    switch (entry.kind) {
    case StartupEntry::Kind::XdgAutostart:
        return setXdgEnabled(entry, enabled);
    case StartupEntry::Kind::SystemdUser:
        return setSystemdEnabled(entry, enabled);
    }
    return tr("Unknown startup entry type.");
}
