#include "optionaltools.h"

#include <QStandardPaths>

namespace optionaltools {
namespace {

struct Definition {
    Tool tool;
    const char *executable;
    const char *purpose;
    const char *packageHint;
};

// The full set of optional tools. Adding one here is all that is needed for
// it to appear in the dependency overview.
constexpr Definition kDefinitions[] = {
    {Tool::NvidiaSmi, "nvidia-smi", "NVIDIA GPU metrics", "nvidia-utils"},
    {Tool::Sensors, "sensors", "Temperature sensors", "lm_sensors"},
    {Tool::Systemctl, "systemctl", "systemd user services", "systemd"},
};

constexpr int kToolCount = static_cast<int>(std::size(kDefinitions));

// Resolved lazily on first access, then frozen. Function-local static so the
// PATH walk cannot run before QCoreApplication exists.
const QVector<ToolInfo> &resolved()
{
    static const QVector<ToolInfo> tools = [] {
        QVector<ToolInfo> result;
        result.reserve(kToolCount);
        for (const Definition &definition : kDefinitions) {
            ToolInfo entry;
            entry.tool = definition.tool;
            entry.executable = QString::fromLatin1(definition.executable);
            entry.purpose = QString::fromLatin1(definition.purpose);
            entry.packageHint = QString::fromLatin1(definition.packageHint);
            entry.absolutePath = QStandardPaths::findExecutable(entry.executable);
            result.append(entry);
        }
        return result;
    }();
    return tools;
}

} // namespace

const ToolInfo &info(Tool tool)
{
    const QVector<ToolInfo> &tools = resolved();
    for (const ToolInfo &entry : tools) {
        if (entry.tool == tool) {
            return entry;
        }
    }
    Q_UNREACHABLE_RETURN(tools.first());
}

bool isAvailable(Tool tool)
{
    return info(tool).available();
}

QString path(Tool tool)
{
    return info(tool).absolutePath;
}

QString missingHint(Tool tool)
{
    const ToolInfo &entry = info(tool);
    return QStringLiteral("%1 need %2 (package %3), which is not installed.")
        .arg(entry.purpose, entry.executable, entry.packageHint);
}

QVector<ToolInfo> all()
{
    return resolved();
}

} // namespace optionaltools
