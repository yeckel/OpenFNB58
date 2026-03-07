#include "BleTransport.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusReply>
#include <QProcess>
#include <QTimer>
#include <QDebug>

#include <limits>
#include <cstdint>

// ── Static constants ──────────────────────────────────────────────────────
const QByteArray BleTransport::BLE_INIT1 = QByteArray::fromHex("aa8100f4");
const QByteArray BleTransport::BLE_INIT2 = QByteArray::fromHex("aa8200a7");
constexpr int BleTransport::PKT_LENS[9];

BleTransport::BleTransport(const QString& mac, QObject* parent)
    : BaseTransport(parent), m_mac(mac)
{}

void BleTransport::requestStop()
{
    m_stop.store(true, std::memory_order_relaxed);
    quit();
}

// ── Connect device via bluetoothctl ──────────────────────────────────────
bool BleTransport::ensureConnected()
{
    const QString devPath = "/org/bluez/hci0/dev_" +
                            m_mac.toUpper().replace(':', '_');
    QDBusInterface props("org.bluez", devPath,
                         "org.freedesktop.DBus.Properties",
                         QDBusConnection::systemBus());
    QDBusMessage r = props.call("Get", "org.bluez.Device1", "Connected");
    if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty()) {
        QVariant v = r.arguments().at(0).value<QDBusVariant>().variant();
        if (v.toBool()) return true;
    }

    emit statusMessage(QString("Connecting to %1…").arg(m_mac));
    QProcess proc;
    proc.start("bluetoothctl", {"connect", m_mac});
    proc.waitForFinished(15000);
    const QString out = proc.readAllStandardOutput();
    if (!out.contains("Connection successful")) {
        emit error(QString("Cannot connect to %1:\n%2").arg(m_mac, out.trimmed()));
        return false;
    }
    QThread::msleep(1500);
    return true;
}

// ── Locate ffe4/ffe9 via BlueZ ObjectManager ─────────────────────────────
// GetManagedObjects returns a{oa{sa{sv}}}.
// QDBusArgument does NOT support extracting into another QDBusArgument;
// nested maps must be opened/closed on the *same* argument object.
bool BleTransport::findGattChars(QString& notifyPath, QString& writePath)
{
    QDBusInterface mgr("org.bluez", "/",
                       "org.freedesktop.DBus.ObjectManager",
                       QDBusConnection::systemBus());
    QDBusMessage reply = mgr.call("GetManagedObjects");
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        emit error("GetManagedObjects failed");
        return false;
    }

    const QString devPrefix = "/org/bluez/hci0/dev_" +
                              m_mac.toUpper().replace(':', '_');

    const QDBusArgument arg = reply.arguments().at(0).value<QDBusArgument>();

    arg.beginMap(); // a{o a{sa{sv}}}
    while (!arg.atEnd()) {
        arg.beginMapEntry();

        QString objPath;
        arg >> objPath;               // object path (o)

        arg.beginMap();               // a{s a{sv}}  — interfaces map
        while (!arg.atEnd()) {
            arg.beginMapEntry();
            QString ifaceName;
            arg >> ifaceName;         // interface name (s)

            if (objPath.startsWith(devPrefix) &&
                ifaceName == "org.bluez.GattCharacteristic1")
            {
                arg.beginMap();       // a{sv}  — properties map
                while (!arg.atEnd()) {
                    arg.beginMapEntry();
                    QString propName;
                    arg >> propName;
                    QDBusVariant propVal;
                    arg >> propVal;
                    if (propName == "UUID") {
                        const QString uuid = propVal.variant().toString();
                        if (uuid == UUID_NOTIFY)     notifyPath = objPath;
                        else if (uuid == UUID_WRITE) writePath  = objPath;
                    }
                    arg.endMapEntry();
                }
                arg.endMap();
            } else {
                // Consume unknown properties without storing
                arg.beginMap();
                while (!arg.atEnd()) {
                    arg.beginMapEntry();
                    QString k; QDBusVariant v;
                    arg >> k >> v;
                    arg.endMapEntry();
                }
                arg.endMap();
            }

            arg.endMapEntry();
        }
        arg.endMap();                 // end interfaces map

        arg.endMapEntry();
    }
    arg.endMap();

    if (notifyPath.isEmpty() || writePath.isEmpty()) {
        emit error(QString("ffe4/ffe9 not found for %1. Is device connected?").arg(m_mac));
        return false;
    }
    return true;
}

// ── Write to a GATT characteristic ───────────────────────────────────────
void BleTransport::writeGatt(const QString& path, const QByteArray& data)
{
    QDBusInterface iface("org.bluez", path,
                         "org.bluez.GattCharacteristic1",
                         QDBusConnection::systemBus());
    iface.call("WriteValue", data, QVariantMap{});
}

// ── D-Bus PropertiesChanged slot ─────────────────────────────────────────
void BleTransport::onPropertiesChanged(const QString& iface,
                                       const QVariantMap& changed,
                                       const QStringList& /*invalidated*/)
{
    if (iface != "org.bluez.GattCharacteristic1") return;

    auto it = changed.find("Value");
    if (it == changed.end()) return;

    // Value comes as QByteArray or as QDBusArgument(ay) depending on Qt
    QByteArray chunk;
    if (it->canConvert<QByteArray>()) {
        chunk = it->toByteArray();
    } else {
        const QDBusArgument arg = it->value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            uchar b; arg >> b;
            chunk.append(static_cast<char>(b));
        }
        arg.endArray();
    }
    if (chunk.isEmpty()) return;

    // Parse BLE stream frames: aa [type] [data_len] [data…] [chk]
    const auto* d = reinterpret_cast<const uint8_t*>(chunk.constData());
    int i = 0, len = chunk.size();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    while (i < len) {
        if (d[i] != 0xAA) { ++i; continue; }
        if (i + 2 >= len)  break;
        uint8_t ptype = d[i + 1];
        uint8_t dlen  = d[i + 2];
        if (ptype < 3 || ptype > 8 || dlen != PKT_LENS[ptype]) { ++i; continue; }
        if (i + (int)dlen + 4 > len) break;

        const uint8_t* p = d + i + 3;
        auto u16 = [](const uint8_t* b) -> double {
            return (b[0] | (uint16_t(b[1]) << 8)) / 1000.0;
        };

        if (ptype == 0x07) {        // VBUS / IBUS
            double vbus = u16(p);
            double ibus = u16(p + 2);
            emit reading(vbus, ibus, vbus * ibus,
                         m_hasDp ? m_lastDp : nan,
                         m_hasDp ? m_lastDn : nan,
                         nan);
        } else if (ptype == 0x06) { // D+ / D-
            m_lastDp = u16(p);
            m_lastDn = u16(p + 2);
            m_hasDp  = true;
        }
        i += (int)dlen + 4;
    }
}

// ── Thread entry ──────────────────────────────────────────────────────────
void BleTransport::run()
{
    if (!ensureConnected()) return;

    QString notifyPath, writePath;
    if (!findGattChars(notifyPath, writePath)) return;

    QDBusConnection bus = QDBusConnection::systemBus();

    // Connect PropertiesChanged — delivered in *this* thread's event loop
    bus.connect("org.bluez", notifyPath,
                "org.freedesktop.DBus.Properties", "PropertiesChanged",
                this,
                SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));

    QDBusInterface ni("org.bluez", notifyPath,
                      "org.bluez.GattCharacteristic1", bus);
    ni.call("StopNotify");
    ni.call("StartNotify");

    writeGatt(writePath, BLE_INIT1);

    auto* initTimer = new QTimer();
    initTimer->setSingleShot(true);
    initTimer->setInterval(2000);
    connect(initTimer, &QTimer::timeout, this,
            [this, writePath]() { writeGatt(writePath, BLE_INIT2); });
    initTimer->start();

    auto* stopTimer = new QTimer();
    stopTimer->setInterval(100);
    connect(stopTimer, &QTimer::timeout, this, [this]() {
        if (m_stop.load(std::memory_order_relaxed)) quit();
    });
    stopTimer->start();

    emit statusMessage(QString("BLE streaming — %1").arg(m_mac));

    exec();

    ni.call("StopNotify");
    bus.disconnect("org.bluez", notifyPath,
                   "org.freedesktop.DBus.Properties", "PropertiesChanged",
                   this,
                   SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    delete initTimer;
    delete stopTimer;
}
