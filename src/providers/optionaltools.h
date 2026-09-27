#pragma once

#include <QString>
#include <QVector>

// Optional command-line tools the app uses when they happen to be installed
// and does without otherwise. Zero-optional-dependency operation is a
// first-class feature: callers must check availability and fall back to an
// inline hint in the UI, never an error dialog and never a crash.
namespace optionaltools {

enum class Tool {
    NvidiaSmi, // NVIDIA GPU metrics (no sysfs equivalent for the blob driver)
    // There is deliberately no lm_sensors entry. One was registered from
    // Phase 1 "for the temperature work in a later phase"; when that work
    // arrived it turned out to need nothing, because everything `sensors`
    // reports comes from /sys/class/hwmon, which is world-readable and needs
    // no tool. The entry was deleted rather than implemented — the sensors
    // page made the zero-dependency claim stronger instead of adding an
    // asterisk to it. See providers/hwmonprovider.h.
    //
    // systemd is universal on the target distro, but treating it as optional
    // costs nothing and keeps the startup tab working (XDG entries only) on a
    // system running some other init.
    Systemctl,
};

struct ToolInfo {
    Tool tool;
    QString executable;   // name looked up on PATH
    QString absolutePath; // empty when the tool is not installed
    QString purpose;      // what this app wants it for, shown in hints
    QString packageHint;  // Arch package providing it, shown in hints

    bool available() const { return !absolutePath.isEmpty(); }
};

// Resolution happens once, on first use, and is cached for the process
// lifetime: PATH lookups hit the filesystem, and a tool installed mid-session
// is not expected to appear without a restart.
const ToolInfo &info(Tool tool);
bool isAvailable(Tool tool);
QString path(Tool tool);

// One-line explanation to show in place of the UI section a missing tool
// would have driven, e.g. "NVIDIA GPU metrics need nvidia-smi (package
// nvidia-utils), which is not installed."
QString missingHint(Tool tool);

// Every known tool with its current resolution, for an "optional
// dependencies" overview.
QVector<ToolInfo> all();

} // namespace optionaltools
