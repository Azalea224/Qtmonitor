#include "networkprovider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QTextStream>

#include <algorithm>

namespace {

QString readSysfsLine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readLine()).trimmed();
}

// Container and virtualisation stacks create interfaces by the dozen, and a
// sidebar row each would bury the adapter the user actually cares about. These
// prefixes are the well-known churn; anything else virtual (wg0, tun0, bond0,
// a VPN) is kept, because those carry traffic somebody chose to create.
bool isEphemeralVirtualInterface(const QString &name)
{
    static const char *prefixes[] = {"veth", "docker", "br-",  "virbr", "vnet",
                                     "vmnet", "kube",  "cni",  "cali",  "flannel",
                                     "tailscale-", "ifb", "dummy"};
    for (const char *prefix : prefixes) {
        if (name.startsWith(QLatin1String(prefix))) {
            return true;
        }
    }
    return false;
}

// ARPHRD_* from <linux/if_arp.h>, as exposed by /sys/class/net/*/type.
QString describeKind(const QString &name, int arpType, bool wireless, bool physical)
{
    if (wireless) {
        return QStringLiteral("Wi-Fi");
    }
    switch (arpType) {
    case 1: // ARPHRD_ETHER
        return physical ? QStringLiteral("Ethernet") : QStringLiteral("Virtual Ethernet");
    case 24:  // ARPHRD_IEEE1394
        return QStringLiteral("FireWire");
    case 65534: // ARPHRD_NONE — tun, wireguard
        return QStringLiteral("Tunnel");
    case 512: // ARPHRD_PPP
        return QStringLiteral("PPP");
    default:
        break;
    }
    Q_UNUSED(name);
    return QStringLiteral("Network");
}

} // namespace

ProcfsNetworkProvider::ProcfsNetworkProvider()
{
    enumerateDevices();
}

bool ProcfsNetworkProvider::isAvailable() const
{
    return QFile::exists(QStringLiteral("/proc/net/dev")) && !m_devices.isEmpty();
}

void ProcfsNetworkProvider::enumerateDevices()
{
    const QDir netDir(QStringLiteral("/sys/class/net"));
    const QStringList names =
        netDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    for (const QString &name : names) {
        // Loopback is always up, always fast and never interesting; it would
        // occupy a permanent sidebar row saying nothing about the machine's
        // connectivity.
        if (name == QLatin1String("lo")) {
            continue;
        }

        const QString base = netDir.absoluteFilePath(name);
        const QString deviceLink =
            QFileInfo(base + QStringLiteral("/device")).symLinkTarget();
        const bool physical = !deviceLink.isEmpty();

        if (!physical && isEphemeralVirtualInterface(name)) {
            continue;
        }

        NetworkDeviceInfo device;
        device.id = name;
        device.physical = physical;
        device.wireless = QFile::exists(base + QStringLiteral("/wireless"))
            || QFile::exists(base + QStringLiteral("/phy80211"));
        device.kind = describeKind(
            name, readSysfsLine(base + QStringLiteral("/type")).toInt(),
            device.wireless, physical);
        device.name = QStringLiteral("%1 (%2)").arg(device.kind, name);

        if (physical) {
            device.busPath = QFileInfo(deviceLink).fileName();
            const QString driverLink =
                QFileInfo(base + QStringLiteral("/device/driver")).symLinkTarget();
            if (!driverLink.isEmpty()) {
                device.driver = QFileInfo(driverLink).fileName();
            }
        }

        m_devices.append(device);
    }

    // Real hardware first: on a machine with tunnels or bridges, the adapter
    // the user is actually looking for should not be below them.
    std::stable_sort(m_devices.begin(), m_devices.end(),
                     [](const NetworkDeviceInfo &a, const NetworkDeviceInfo &b) {
                         return a.physical && !b.physical;
                     });
}

QVector<NetworkSnapshot> ProcfsNetworkProvider::sample()
{
    // As in the disk provider: the first call only establishes a baseline.
    const bool first = !m_elapsed.isValid();
    const double elapsedMsec = first ? 0.0 : static_cast<double>(m_elapsed.restart());
    if (first) {
        m_elapsed.start();
    }

    QHash<QString, NetworkSnapshot> byName;

    QFile file(QStringLiteral("/proc/net/dev"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        QString line;
        while (!(line = in.readLine()).isNull()) {
            // "  eth0: 123 45 ..." — the name is everything before the colon,
            // which is the only reliable separator: the field is right-aligned
            // in a fixed width, so a short name has no space before it and a
            // long one has none after.
            const int colon = line.indexOf(QLatin1Char(':'));
            if (colon < 0) {
                continue;
            }
            const QString name = line.left(colon).trimmed();
            const QStringList values =
                line.mid(colon + 1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            // 8 receive columns then 8 transmit columns.
            if (values.size() < 16) {
                continue;
            }

            NetworkSnapshot snapshot;
            snapshot.id = name;
            snapshot.receiveTotalBytes = values.at(0).toULongLong();
            snapshot.receivePackets = values.at(1).toULongLong();
            snapshot.receiveErrors = values.at(2).toULongLong();
            snapshot.receiveDrops = values.at(3).toULongLong();
            snapshot.sendTotalBytes = values.at(8).toULongLong();
            snapshot.sendPackets = values.at(9).toULongLong();
            snapshot.sendErrors = values.at(10).toULongLong();
            snapshot.sendDrops = values.at(11).toULongLong();
            byName.insert(name, snapshot);
        }
    }

    // One netlink round trip for every interface's addresses and MAC, rather
    // than parsing them out of sysfs and /proc/net/if_inet6 by hand.
    // QNetworkInterface lives in Qt6::Network, which ships inside qt6-base
    // alongside Widgets, so this costs no extra package.
    QHash<QString, QNetworkInterface> interfaces;
    const QList<QNetworkInterface> allInterfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &interface : allInterfaces) {
        interfaces.insert(interface.name(), interface);
    }

    QVector<NetworkSnapshot> snapshots;
    snapshots.reserve(m_devices.size());

    for (const NetworkDeviceInfo &device : m_devices) {
        NetworkSnapshot snapshot = byName.value(device.id);
        snapshot.id = device.id;

        const Counters previous = m_previous.value(device.id);
        if (previous.valid && elapsedMsec > 0.0
            && snapshot.receiveTotalBytes >= previous.receiveBytes
            && snapshot.sendTotalBytes >= previous.sendBytes) {
            const double seconds = elapsedMsec / 1000.0;
            snapshot.receiveBytesPerSec =
                (snapshot.receiveTotalBytes - previous.receiveBytes) / seconds;
            snapshot.sendBytesPerSec =
                (snapshot.sendTotalBytes - previous.sendBytes) / seconds;
        }

        Counters counters;
        counters.receiveBytes = snapshot.receiveTotalBytes;
        counters.sendBytes = snapshot.sendTotalBytes;
        counters.valid = true;
        m_previous.insert(device.id, counters);

        const QString base = QStringLiteral("/sys/class/net/") + device.id;
        snapshot.state = readSysfsLine(base + QStringLiteral("/operstate"));
        snapshot.mtu = readSysfsLine(base + QStringLiteral("/mtu")).toInt();
        snapshot.duplex = readSysfsLine(base + QStringLiteral("/duplex"));
        // speed reads back as -1, or EINVAL, on a link that has not negotiated
        // one — a down Ethernet port or any wireless interface.
        bool speedOk = false;
        const int speed =
            readSysfsLine(base + QStringLiteral("/speed")).toInt(&speedOk);
        snapshot.linkSpeedMbps = (speedOk && speed > 0) ? speed : -1;

        const auto interface = interfaces.constFind(device.id);
        if (interface != interfaces.cend()) {
            snapshot.macAddress = interface->hardwareAddress();
            snapshot.up = interface->flags().testFlag(QNetworkInterface::IsUp)
                && interface->flags().testFlag(QNetworkInterface::IsRunning);
            const QList<QNetworkAddressEntry> entries = interface->addressEntries();
            for (const QNetworkAddressEntry &entry : entries) {
                const QHostAddress address = entry.ip();
                if (address.protocol() == QAbstractSocket::IPv4Protocol) {
                    snapshot.addresses.prepend(address.toString());
                } else {
                    // Link-local addresses carry a "%iface" scope suffix that
                    // adds nothing next to the interface's own row.
                    QString text = address.toString();
                    const int scope = text.indexOf(QLatin1Char('%'));
                    if (scope >= 0) {
                        text.truncate(scope);
                    }
                    snapshot.addresses.append(text);
                }
            }
        } else {
            snapshot.up = snapshot.state == QLatin1String("up");
        }

        snapshots.append(snapshot);
    }

    return snapshots;
}
