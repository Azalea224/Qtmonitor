#include "nvidiagpuprovider.h"

#include <QProcess>
#include <QStringList>

#include "optionaltools.h"

namespace {

// Fields pulled every tick, in this exact order; parsing is positional.
const QStringList kQueryFields = {
    QStringLiteral("pci.bus_id"),
    QStringLiteral("utilization.gpu"),
    QStringLiteral("memory.used"),
    QStringLiteral("memory.total"),
    QStringLiteral("temperature.gpu"),
    QStringLiteral("power.draw"),
    QStringLiteral("clocks.current.graphics"),
    QStringLiteral("utilization.encoder"),
    QStringLiteral("utilization.decoder"),
    QStringLiteral("fan.speed"),
};

// A process that hangs (a wedged driver is the usual cause) must not be
// relaunched every second until the machine drowns in them.
constexpr qint64 kProcessTimeoutMsec = 5000;

// nvidia-smi renders unsupported fields as "[N/A]" or "[Not Supported]"
// rather than omitting them.
double parseOptionalDouble(const QString &field)
{
    bool ok = false;
    const double value = field.trimmed().toDouble(&ok);
    return ok ? value : -1.0;
}

} // namespace

NvidiaSmiGpuProvider::NvidiaSmiGpuProvider(QObject *parent)
    : QObject(parent)
    , m_queryProcess(new QProcess(this))
    , m_pmonProcess(new QProcess(this))
{
    if (!optionaltools::isAvailable(optionaltools::Tool::NvidiaSmi)) {
        return;
    }

    connect(m_queryProcess, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        parseQueryOutput(m_queryProcess->readAllStandardOutput());
    });
    connect(m_pmonProcess, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        parsePmonOutput(m_pmonProcess->readAllStandardOutput());
    });

    discoverDevices();
}

QString NvidiaSmiGpuProvider::normalizeBusId(const QString &busId)
{
    QStringList parts = busId.trimmed().toLower().split(QLatin1Char(':'));
    if (parts.size() < 2) {
        return busId.trimmed().toLower();
    }
    if (parts.first().size() > 4) {
        parts.first() = parts.first().right(4);
    }
    return parts.join(QLatin1Char(':'));
}

void NvidiaSmiGpuProvider::discoverDevices()
{
    // The only synchronous call in this class. It runs once, before the UI is
    // built, so the sidebar has its GPU rows on the first frame rather than
    // growing a second later.
    QProcess probe;
    probe.start(optionaltools::path(optionaltools::Tool::NvidiaSmi),
                {QStringLiteral("--query-gpu=pci.bus_id,name,memory.total,driver_version"),
                 QStringLiteral("--format=csv,noheader,nounits")});
    if (!probe.waitForFinished(3000) || probe.exitStatus() != QProcess::NormalExit
        || probe.exitCode() != 0) {
        return;
    }

    const QList<QByteArray> lines = probe.readAllStandardOutput().split('\n');
    for (const QByteArray &line : lines) {
        const QString text = QString::fromLatin1(line).trimmed();
        if (text.isEmpty()) {
            continue;
        }
        const QStringList fields = text.split(QLatin1Char(','));
        if (fields.size() < 3) {
            continue;
        }

        GpuDeviceInfo info;
        info.id = normalizeBusId(fields.at(0));
        info.name = fields.at(1).trimmed();
        // nounits reports memory in MiB.
        info.memTotalBytes =
            static_cast<quint64>(qMax(0.0, parseOptionalDouble(fields.at(2)))) * 1024ull
            * 1024ull;
        info.driver = fields.size() > 3
            ? QStringLiteral("nvidia %1").arg(fields.at(3).trimmed())
            : QStringLiteral("nvidia");
        info.metricsAvailable = true;
        m_devices.append(info);
    }
    m_available = !m_devices.isEmpty();
}

bool NvidiaSmiGpuProvider::isAvailable() const
{
    return m_available;
}

QVector<GpuDeviceInfo> NvidiaSmiGpuProvider::devices() const
{
    return m_devices;
}

void NvidiaSmiGpuProvider::setProcessSamplingEnabled(bool enabled)
{
    if (m_processSamplingEnabled == enabled) {
        return;
    }
    m_processSamplingEnabled = enabled;
    if (!enabled) {
        m_processUsage.clear();
    }
}

void NvidiaSmiGpuProvider::startQuery()
{
    if (m_queryProcess->state() != QProcess::NotRunning) {
        if (m_queryStarted.isValid() && m_queryStarted.elapsed() > kProcessTimeoutMsec) {
            m_queryProcess->kill();
        }
        return;
    }
    m_queryStarted.start();
    m_queryProcess->start(
        optionaltools::path(optionaltools::Tool::NvidiaSmi),
        {QStringLiteral("--query-gpu=") + kQueryFields.join(QLatin1Char(',')),
         QStringLiteral("--format=csv,noheader,nounits")});
}

void NvidiaSmiGpuProvider::startProcessQuery()
{
    if (m_pmonProcess->state() != QProcess::NotRunning) {
        if (m_pmonStarted.isValid() && m_pmonStarted.elapsed() > kProcessTimeoutMsec) {
            m_pmonProcess->kill();
        }
        return;
    }
    m_pmonStarted.start();
    // pmon is the only nvidia-smi mode reporting per-process utilization;
    // --query-compute-apps covers CUDA clients only and would miss every
    // graphics process.
    m_pmonProcess->start(optionaltools::path(optionaltools::Tool::NvidiaSmi),
                         {QStringLiteral("pmon"), QStringLiteral("-c"),
                          QStringLiteral("1")});
}

void NvidiaSmiGpuProvider::parseQueryOutput(const QByteArray &output)
{
    QVector<GpuSnapshot> snapshots;
    const QList<QByteArray> lines = output.split('\n');

    for (const QByteArray &line : lines) {
        const QString text = QString::fromLatin1(line).trimmed();
        if (text.isEmpty()) {
            continue;
        }
        const QStringList fields = text.split(QLatin1Char(','));
        if (fields.size() < kQueryFields.size()) {
            continue;
        }

        GpuSnapshot snapshot;
        snapshot.id = normalizeBusId(fields.at(0));
        snapshot.busyPercent = parseOptionalDouble(fields.at(1));
        const double usedMiB = parseOptionalDouble(fields.at(2));
        const double totalMiB = parseOptionalDouble(fields.at(3));
        snapshot.memUsedBytes =
            usedMiB > 0 ? static_cast<quint64>(usedMiB) * 1024ull * 1024ull : 0;
        snapshot.memTotalBytes =
            totalMiB > 0 ? static_cast<quint64>(totalMiB) * 1024ull * 1024ull : 0;
        snapshot.temperatureC = parseOptionalDouble(fields.at(4));
        snapshot.powerWatts = parseOptionalDouble(fields.at(5));
        snapshot.clockMhz = parseOptionalDouble(fields.at(6));

        // The card reports encode and decode separately; the video column is
        // whichever block is busier, matching how the engine categories are
        // reduced on the DRM side.
        const double encoder = parseOptionalDouble(fields.at(7));
        const double decoder = parseOptionalDouble(fields.at(8));
        if (encoder >= 0.0 || decoder >= 0.0) {
            snapshot.videoPercent = qMax(encoder, decoder);
        }
        // "[N/A]" on a card with no fan of its own, which parses to -1.
        snapshot.fanPercent = parseOptionalDouble(fields.at(9));

        snapshots.append(snapshot);
    }

    // A failed or truncated run keeps the previous readings rather than
    // blanking the graphs.
    if (!snapshots.isEmpty()) {
        m_snapshots = std::move(snapshots);
    }
}

void NvidiaSmiGpuProvider::parsePmonOutput(const QByteArray &output)
{
    // pmon's column set differs between driver generations (jpg and ofa are
    // recent additions), so the header is parsed for names rather than
    // trusting fixed positions.
    QStringList columns;
    QHash<int, GpuProcessUsage> usage;

    const QList<QByteArray> lines = output.split('\n');
    for (const QByteArray &line : lines) {
        const QString text = QString::fromLatin1(line);
        if (text.trimmed().isEmpty()) {
            continue;
        }

        if (text.startsWith(QLatin1Char('#'))) {
            if (columns.isEmpty()) {
                // "# gpu  pid  type  sm  mem  enc  dec  jpg  ofa  command"
                columns = text.mid(1).simplified().split(QLatin1Char(' '));
            }
            continue;
        }
        if (columns.isEmpty()) {
            continue;
        }

        const QStringList fields = text.simplified().split(QLatin1Char(' '));
        const auto fieldFor = [&](const QString &name) -> QString {
            const int column = columns.indexOf(name);
            if (column < 0 || column >= fields.size()) {
                return {};
            }
            return fields.at(column);
        };

        bool pidOk = false;
        const int pid = fieldFor(QStringLiteral("pid")).toInt(&pidOk);
        if (!pidOk || pid <= 0) {
            continue;
        }

        // An idle sample prints "-" for every engine. The process still holds
        // a GPU context, so that is a measured zero.
        const auto percentFor = [&](const QString &name) {
            bool ok = false;
            const double value = fieldFor(name).toDouble(&ok);
            return ok ? value : 0.0;
        };

        GpuProcessUsage entry;
        entry.graphicsPercent = percentFor(QStringLiteral("sm"));
        entry.videoPercent = qMax(
            qMax(percentFor(QStringLiteral("enc")), percentFor(QStringLiteral("dec"))),
            qMax(percentFor(QStringLiteral("jpg")), percentFor(QStringLiteral("ofa"))));

        // A process can appear once per GPU it uses.
        GpuProcessUsage &slot = usage[pid];
        slot.graphicsPercent = qMin(100.0, qMax(slot.graphicsPercent, 0.0)
                                        + entry.graphicsPercent);
        slot.videoPercent =
            qMin(100.0, qMax(slot.videoPercent, 0.0) + entry.videoPercent);
    }

    m_processUsage = std::move(usage);
}

QVector<GpuSnapshot> NvidiaSmiGpuProvider::sample()
{
    if (!m_available) {
        return {};
    }
    startQuery();
    if (m_processSamplingEnabled) {
        startProcessQuery();
    }
    return m_snapshots;
}

QHash<int, GpuProcessUsage> NvidiaSmiGpuProvider::sampleProcesses()
{
    return m_processSamplingEnabled ? m_processUsage : QHash<int, GpuProcessUsage>{};
}
