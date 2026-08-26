#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

// A network interface as the UI presents it. Identity is the kernel name,
// which is what /proc/net/dev, /sys/class/net and netlink all agree on.
struct NetworkDeviceInfo {
    QString id;      // "enp77s0"
    QString name;    // "Ethernet (enp77s0)"
    QString kind;    // "Ethernet", "Wi-Fi", "Virtual", ...
    QString driver;  // kernel module behind it, when there is real hardware
    QString busPath; // PCI/USB address, empty for virtual interfaces
    bool wireless = false;
    bool physical = false; // backed by a device in sysfs
};

// Live per-interface counters. Rates come from /proc/net/dev deltas over the
// measured elapsed time; the rest is state that genuinely changes at runtime
// (a cable pulled, an address gained), so it belongs in the snapshot rather
// than the device description.
struct NetworkSnapshot {
    QString id;
    double receiveBytesPerSec = 0.0;
    double sendBytesPerSec = 0.0;
    quint64 receiveTotalBytes = 0;
    quint64 sendTotalBytes = 0;
    quint64 receivePackets = 0;
    quint64 sendPackets = 0;
    quint64 receiveErrors = 0;
    quint64 sendErrors = 0;
    quint64 receiveDrops = 0;
    quint64 sendDrops = 0;
    QString state;         // operstate: "up", "down", "unknown", ...
    bool up = false;       // carrier present and administratively up
    int linkSpeedMbps = -1;// -1 when the link reports no negotiated speed
    QString duplex;
    int mtu = 0;
    QString macAddress;
    QStringList addresses; // IPv4 then IPv6, as configured right now
};

// Abstract network metrics source.
class INetworkProvider
{
public:
    virtual ~INetworkProvider() = default;
    virtual bool isAvailable() const = 0;
    // Enumerated once at construction, like disks and GPUs. An interface that
    // appears later (a USB adapter, a VPN coming up) needs a restart to get
    // its own page.
    virtual QVector<NetworkDeviceInfo> devices() const = 0;
    virtual QVector<NetworkSnapshot> sample() = 0;
};

// Default provider: /proc/net/dev for the counters, /sys/class/net for the
// hardware description and link state, QNetworkInterface for the addresses.
class ProcfsNetworkProvider final : public INetworkProvider
{
public:
    ProcfsNetworkProvider();

    bool isAvailable() const override;
    QVector<NetworkDeviceInfo> devices() const override { return m_devices; }
    QVector<NetworkSnapshot> sample() override;

private:
    struct Counters {
        quint64 receiveBytes = 0;
        quint64 sendBytes = 0;
        bool valid = false;
    };

    void enumerateDevices();

    QVector<NetworkDeviceInfo> m_devices;
    QHash<QString, Counters> m_previous;
    QElapsedTimer m_elapsed;
};
