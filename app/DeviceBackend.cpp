#include "DeviceBackend.h"
#include "UsbTransport.h"
#include "BleTransport.h"
#include "XlsxWriter.h"

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QFileInfo>
#include <QUrl>
#include <QtMath>
#include <cmath>
#include <limits>

#ifdef FNB58_HAVE_BLUETOOTH
#  include <QBluetoothDeviceDiscoveryAgent>
#  include <QBluetoothDeviceInfo>
#endif
#include <QProcess>

// ── Helpers ───────────────────────────────────────────────────────────────
static double nowSecs() {
    return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

static QString urlToPath(const QString& urlOrPath) {
    if (urlOrPath.startsWith("file:///"))
        return urlOrPath.mid(7);       // file:///home/… → /home/…
    if (urlOrPath.startsWith("file://"))
        return urlOrPath.mid(7);
    return urlOrPath;
}

// ── Constructor / destructor ──────────────────────────────────────────────
DeviceBackend::DeviceBackend(QObject* parent) : QObject(parent)
{
    m_durationTimer = new QTimer(this);
    m_durationTimer->setInterval(1000);
    connect(m_durationTimer, &QTimer::timeout, this, &DeviceBackend::onDurationTick);
}

DeviceBackend::~DeviceBackend()
{
    cleanupTransport();
}

// ── Protocol detection & trigger ──────────────────────────────────────────

QList<DeviceBackend::ProtocolInfo> DeviceBackend::allProtocols()
{
    return {
        { 1,  "QC 2.0 5V",    5000,  {5000} },
        { 2,  "QC 2.0 9V",    9000,  {9000} },
        { 3,  "QC 2.0 12V",  12000,  {12000} },
        { 4,  "QC 2.0 20V",  20000,  {20000} },
        { 5,  "QC 3.0",       9000,  {3600,4200,4600,5000,5400,6000,6500,7000,7500,8000,8500,9000,9500,10000,11000,12000,13000,14000,15000,16000,17000,18000,19000,20000} },
        { 6,  "FCP/AFC 9V",   9000,  {9000} },
        { 7,  "FCP/AFC 12V", 12000,  {12000} },
        { 8,  "Huawei FCP",   9000,  {9000, 12000} },
        { 9,  "Huawei SCP",   5000,  {4000,4500,5000,5500,6000,6500,7000,7500,8000} },
        { 10, "Samsung 2A",   5000,  {5000} },
        { 11, "Samsung AFC",  9000,  {9000, 12000} },
        { 12, "Apple 2.1A",   5000,  {5000} },
        { 13, "Apple 2.4A",   5000,  {5000} },
        { 14, "PD / MTK",     9000,  {5000, 9000, 12000, 15000, 20000} },
    };
}

QString DeviceBackend::inferProtocol(double dp, double dn, double vbus)
{
    auto near = [](double v, double t) { return qAbs(v - t) < 0.18; };

    if (dp < 0.35 && dn < 0.35)
        return QStringLiteral("USB SDP");

    // Equal D+/D− (charger sets both lines to same voltage)
    if (near(dp, dn)) {
        if (near(dp, 2.0))                  return QStringLiteral("Apple 1A");
        if (near(dp, 2.7) && near(dn, 2.0)) return QStringLiteral("Apple 2.1A");
        if (near(dp, 2.0) && near(dn, 2.7)) return QStringLiteral("Apple 2.4A");
        if (dp > 0.5 && dp < 1.8)           return QStringLiteral("DCP / Samsung");
        if (dp >= 1.8 && dp < 2.3)          return QStringLiteral("Apple 1A");
        return QString("DCP (%1V)").arg(dp, 0, 'f', 2);
    }

    // QC 2.0 codes
    if (near(dp, 0.6) && dn < 0.35)         return QStringLiteral("QC 2.0 5V");
    if (near(dp, 3.3) && near(dn, 0.6))     return QStringLiteral("QC 2.0 9V");
    if (near(dp, 0.6) && near(dn, 0.6))     return QStringLiteral("QC 2.0 12V");
    if (near(dp, 3.3) && near(dn, 3.3))     return QStringLiteral("QC 2.0 20V");
    if (near(dp, 0.6) && dn > 0.0 && dn < 3.6)
        return QStringLiteral("QC 3.0");

    // Huawei FCP handshake
    if (dp > 0.2 && dp < 0.45 && dn < 0.3) return QStringLiteral("Huawei FCP");

    // USB PD (CC pins used, D+/D- idle but VBUS elevated)
    if (vbus > 5.5)
        return QString("USB PD (%1V)").arg(qRound(vbus));

    return QString("Unknown (D+=%1V D-=%2V)").arg(dp, 0,'f',2).arg(dn, 0,'f',2);
}

void DeviceBackend::sendTrigger(int protoId, int voltage_mV)
{
    if (!m_transport) return;

    // Build 64-byte trigger command with CRC-8
    auto crc8 = [](const uint8_t* data, int len) -> uint8_t {
        uint8_t crc = 0x42;
        for (int i = 0; i < len; ++i) {
            crc ^= data[i];
            for (int b = 0; b < 8; ++b)
                crc = (crc & 0x80) ? ((crc << 1) ^ 0x39) : (crc << 1);
        }
        return crc;
    };

    QByteArray cmd(64, '\0');
    cmd[0] = static_cast<char>(0xAA);
    cmd[1] = static_cast<char>(0x85);   // CMD_TRIGGER (best-guess)
    cmd[2] = static_cast<char>(protoId);
    int v10 = voltage_mV / 10;          // 10 mV resolution
    cmd[3] = static_cast<char>(v10 & 0xFF);
    cmd[4] = static_cast<char>((v10 >> 8) & 0xFF);
    const uint8_t* inner = reinterpret_cast<const uint8_t*>(cmd.constData()) + 1;
    cmd[63] = static_cast<char>(crc8(inner, 62));

    QMetaObject::invokeMethod(m_transport, "sendCommand",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, cmd));
    emit statusChanged(QString("Trigger sent: proto %1 @ %2 V")
                       .arg(protoId).arg(voltage_mV / 1000.0, 0, 'f', 1));
}

void DeviceBackend::releaseTrigger()
{
    if (!m_transport) return;

    auto crc8 = [](const uint8_t* data, int len) -> uint8_t {
        uint8_t crc = 0x42;
        for (int i = 0; i < len; ++i) {
            crc ^= data[i];
            for (int b = 0; b < 8; ++b)
                crc = (crc & 0x80) ? ((crc << 1) ^ 0x39) : (crc << 1);
        }
        return crc;
    };

    QByteArray cmd(64, '\0');
    cmd[0] = static_cast<char>(0xAA);
    cmd[1] = static_cast<char>(0x86);   // CMD_RELEASE (best-guess)
    const uint8_t* inner = reinterpret_cast<const uint8_t*>(cmd.constData()) + 1;
    cmd[63] = static_cast<char>(crc8(inner, 62));

    QMetaObject::invokeMethod(m_transport, "sendCommand",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, cmd));
    emit statusChanged("Trigger released");
}

// ── Transport lifecycle ───────────────────────────────────────────────────
void DeviceBackend::start(const QString& transport, const QString& address)
{
    if (m_running) return;

    cleanupTransport();

    m_readings.clear();
    m_energyWh   = 0.0;
    m_energyBase = 0.0;
    m_startTime  = nowSecs();
    m_lastT      = -1.0;
    m_lastPower  = 0.0;

    if (transport == "usb") {
        m_transport = new UsbTransport(this);
    } else {
#ifdef FNB58_HAVE_BLUETOOTH
        const QString mac = address.trimmed();
        QBluetoothDeviceInfo info = m_bleDeviceCache.value(mac);
        if (!info.isValid())
            info = QBluetoothDeviceInfo(QBluetoothAddress(mac), mac, 0);
        m_transport = new BleTransport(info, this);
#else
        emit statusChanged("BLE not available: rebuild with Qt Bluetooth (qtconnectivity).");
        emit runningChanged(false);
        return;
#endif
    }

    connect(m_transport, &BaseTransport::reading,
            this, &DeviceBackend::onReading, Qt::QueuedConnection);
    connect(m_transport, &BaseTransport::error,
            this, &DeviceBackend::onTransportError, Qt::QueuedConnection);
    connect(m_transport, &BaseTransport::statusMessage,
            this, &DeviceBackend::onStatusMessage, Qt::QueuedConnection);

    m_transport->start();
    m_running = true;
    m_durationTimer->start();

    emit runningChanged(true);
    emit statusChanged("Connecting…");
}

void DeviceBackend::stop()
{
    if (!m_running) return;

    m_running = false;
    m_durationTimer->stop();
    cleanupTransport();

    emit runningChanged(false);
    emit statusChanged(
        QString("Stopped — %1 samples, %2 mWh")
            .arg(m_readings.size())
            .arg(m_energyWh * 1000, 0, 'f', 2));
}

void DeviceBackend::cleanupTransport()
{
    if (!m_transport) return;
    m_transport->requestStop();
    m_transport->quit();
    if (!m_transport->wait(3000))
        m_transport->terminate();
    delete m_transport;
    m_transport = nullptr;
}

// ── Slots from transport ──────────────────────────────────────────────────
void DeviceBackend::onReading(double vbus, double ibus, double power,
                               double dp, double dn, double temp)
{
    const double t = nowSecs() - m_startTime;

    DataRecord rec;
    rec.timestamp = t;
    rec.vbus      = vbus;
    rec.ibus      = ibus;
    rec.power     = power;
    rec.dp        = dp;
    rec.dn        = dn;
    rec.temp      = temp;

    // Trapezoidal energy integration
    if (m_lastT >= 0.0) {
        double dt = t - m_lastT;
        if (dt > 0 && dt < 10.0) // guard against huge gaps
            m_energyWh += 0.5 * (power + m_lastPower) * dt / 3600.0;
    }
    m_lastT     = t;
    m_lastPower = power;
    rec.energyCum = m_energyWh;

    m_readings.append(rec);

    // Protocol detection (update only if changed to avoid QML spam)
    const QString proto = inferProtocol(dp, dn, vbus);
    if (proto != m_protocolName) {
        m_protocolName = proto;
        emit protocolChanged(m_protocolName);
    }

    emit newReading(t, vbus, ibus, power, dp, dn, temp);
    emit energyChanged(m_energyWh - m_energyBase);
    emit sampleCountChanged(m_readings.size());
}

void DeviceBackend::onTransportError(const QString& msg)
{
    emit errorOccurred(msg);
    stop();
}

void DeviceBackend::onStatusMessage(const QString& msg)
{
    emit statusChanged(msg);
}

void DeviceBackend::onDurationTick()
{
    auto dt = static_cast<qint64>(nowSecs() - m_startTime);
    m_duration = formatDuration(dt);
    emit durationChanged(m_duration);
}

// ── Range measurement ─────────────────────────────────────────────────────
QVariantMap DeviceBackend::measureRange(double tStart, double tEnd) const
{
    QVariantMap r;
    if (tStart > tEnd) std::swap(tStart, tEnd);

    double sumVbus = 0, sumIbus = 0, sumPower = 0;
    double peakVbus = 0, peakIbus = 0, peakPower = 0;
    double minTemp = std::numeric_limits<double>::infinity();
    double maxTemp = -std::numeric_limits<double>::infinity();
    double energyStart = -1, energyEnd = -1;
    int count = 0;

    for (const auto& rec : m_readings) {
        if (rec.timestamp < tStart || rec.timestamp > tEnd) continue;
        sumVbus  += rec.vbus;
        sumIbus  += rec.ibus;
        sumPower += rec.power;
        peakVbus  = std::max(peakVbus,  rec.vbus);
        peakIbus  = std::max(peakIbus,  rec.ibus);
        peakPower = std::max(peakPower, rec.power);
        if (std::isfinite(rec.temp)) {
            minTemp = std::min(minTemp, rec.temp);
            maxTemp = std::max(maxTemp, rec.temp);
        }
        if (energyStart < 0) energyStart = rec.energyCum;
        energyEnd = rec.energyCum;
        ++count;
    }

    r["sampleCount"] = count;
    r["duration"]    = tEnd - tStart;
    if (count > 0) {
        r["meanVbus"]   = sumVbus  / count;
        r["meanIbus"]   = sumIbus  / count;
        r["meanPower"]  = sumPower / count;
        r["peakVbus"]   = peakVbus;
        r["peakIbus"]   = peakIbus;
        r["peakPower"]  = peakPower;
        double dE = energyEnd - energyStart;
        r["energyWh"]   = dE;
        r["energyMWh"]  = dE * 1000.0;
        r["energyMAs"]  = dE * 3600.0 * 1000.0; // mA·h → mA·s? No: Wh→mAh needs /V
        // mAh = mWh / meanV (approximate)
        double meanV = sumVbus / count;
        r["energyMAh"]  = meanV > 0.01 ? (dE * 1000.0 / meanV) : 0.0;
    }
    if (std::isfinite(minTemp)) {
        r["minTemp"] = minTemp;
        r["maxTemp"] = maxTemp;
    }
    return r;
}

// ── BLE device scan ───────────────────────────────────────────────────────
void DeviceBackend::scanBleDevices()
{
#ifdef FNB58_HAVE_BLUETOOTH
    emit statusChanged("Scanning for BLE devices (5 s)…");

    // ── Step 1: seed cache from system's known-device list ─────────────────
    // QBluetoothDeviceDiscoveryAgent only finds devices that are actively
    // advertising right now.  On Linux, "bluetoothctl devices" returns every
    // device BlueZ has ever seen (paired, bonded, or just previously scanned).
    // We pre-populate the cache so those devices are always selectable.
#ifdef Q_OS_LINUX
    QProcess btctl;
    btctl.start("bluetoothctl", {"devices"});
    if (btctl.waitForFinished(3000)) {
        for (const QString& line :
             QString(btctl.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
            // Format: "Device BA:03:18:7A:23:DF FNB58-038059"
            const QStringList parts = line.trimmed().split(' ', Qt::SkipEmptyParts);
            if (parts.size() >= 2 && parts[0] == "Device") {
                const QString mac  = parts[1];
                const QString name = parts.size() >= 3 ? parts.mid(2).join(' ') : mac;
                if (!m_bleDeviceCache.contains(mac))
                    m_bleDeviceCache.insert(
                        mac, QBluetoothDeviceInfo(QBluetoothAddress(mac), name, 0));
            }
        }
    }
#endif

    // ── Step 2: live 5-second BLE scan (updates names, finds new devices) ──
    auto* agent = new QBluetoothDeviceDiscoveryAgent(this);
    agent->setLowEnergyDiscoveryTimeout(5000);

    connect(agent, &QBluetoothDeviceDiscoveryAgent::finished,
            this, [this, agent]() {
        for (const auto& info : agent->discoveredDevices()) {
            if (info.coreConfigurations()
                    & QBluetoothDeviceInfo::LowEnergyCoreConfiguration)
                m_bleDeviceCache.insert(info.address().toString(), info);
        }

        // Emit the full cache (known + just-scanned)
        QStringList result;
        for (auto it = m_bleDeviceCache.constBegin();
             it != m_bleDeviceCache.constEnd(); ++it) {
            const QString name = it.value().name().isEmpty() ? it.key()
                                                             : it.value().name();
            result << QString("%1 (%2)").arg(name, it.key());
        }

        if (result.isEmpty())
            emit statusChanged("No BLE devices found. Make sure FNB58 is powered on.");
        else
            emit statusChanged(QString("Found %1 BLE device(s)").arg(result.size()));
        emit bleDevicesFound(result);
        agent->deleteLater();
    });

    connect(agent, &QBluetoothDeviceDiscoveryAgent::errorOccurred,
            this, [this, agent](QBluetoothDeviceDiscoveryAgent::Error) {
        // Scan failed, but still emit whatever we got from bluetoothctl
        QStringList result;
        for (auto it = m_bleDeviceCache.constBegin();
             it != m_bleDeviceCache.constEnd(); ++it) {
            const QString name = it.value().name().isEmpty() ? it.key()
                                                             : it.value().name();
            result << QString("%1 (%2)").arg(name, it.key());
        }
        emit statusChanged(result.isEmpty()
            ? QString("BLE scan error: %1").arg(agent->errorString())
            : QString("BLE scan error — showing %1 cached device(s)").arg(result.size()));
        emit bleDevicesFound(result);
        agent->deleteLater();
    });

    agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
#else
    emit statusChanged("BLE not available: rebuild with Qt Bluetooth (qtconnectivity).");
    emit bleDevicesFound({});
#endif
}

// ── Reset energy marker ───────────────────────────────────────────────────
void DeviceBackend::resetEnergy()
{
    m_energyBase = m_energyWh;
    emit energyChanged(0.0);
    emit statusChanged("Energy marker set");
}

// ── CSV export ────────────────────────────────────────────────────────────
void DeviceBackend::exportCsv(const QString& rawPath)
{
    const QString path = urlToPath(rawPath);
    if (m_readings.isEmpty()) {
        emit statusChanged("No data to export.");
        return;
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit statusChanged("Cannot open file: " + path);
        return;
    }

    QTextStream out(&f);
    out << "time_s,vbus_V,ibus_A,power_W,dp_V,dn_V,temp_C,energy_Wh\n";
    for (const auto& r : m_readings) {
        auto nan2str = [](double v) -> QString {
            return std::isfinite(v) ? QString::number(v, 'f', 5) : "";
        };
        out << QString::number(r.timestamp, 'f', 3) << ','
            << QString::number(r.vbus,  'f', 5) << ','
            << QString::number(r.ibus,  'f', 5) << ','
            << QString::number(r.power, 'f', 5) << ','
            << nan2str(r.dp)   << ','
            << nan2str(r.dn)   << ','
            << nan2str(r.temp) << ','
            << QString::number(r.energyCum, 'f', 8) << '\n';
    }

    emit statusChanged("CSV saved → " + QFileInfo(path).fileName());
}

// ── Excel export ─────────────────────────────────────────────────────────
void DeviceBackend::exportExcel(const QString& rawPath)
{
    const QString path = urlToPath(rawPath);
    if (m_readings.isEmpty()) {
        emit statusChanged("No data to export.");
        return;
    }

    XlsxWriter writer;
    writer.setTitle("FNB58 Power Meter — Session");

    // Data sheet
    writer.addSheet("Data");
    writer.setHeaders({"Time (s)", "VBUS (V)", "IBUS (A)", "Power (W)",
                       "D+ (V)", "D− (V)", "Temp (°C)", "Energy (Wh)"});
    for (const auto& r : m_readings) {
        writer.addRow({r.timestamp, r.vbus, r.ibus, r.power,
                       r.dp, r.dn, r.temp, r.energyCum});
    }

    // Summary sheet
    writer.addSheet("Summary");
    writer.addSummaryRow("Samples",    static_cast<double>(m_readings.size()));
    writer.addSummaryRow("Duration (s)",
                         m_readings.isEmpty() ? 0 : m_readings.last().timestamp);
    writer.addSummaryRow("Total Energy (Wh)", m_energyWh);
    writer.addSummaryRow("Total Energy (mWh)", m_energyWh * 1000.0);

    auto maxOf = [&](auto field) {
        double mx = 0;
        for (const auto& r : m_readings) mx = std::max(mx, r.*field);
        return mx;
    };
    writer.addSummaryRow("Peak VBUS (V)",  maxOf(&DataRecord::vbus));
    writer.addSummaryRow("Peak IBUS (A)",  maxOf(&DataRecord::ibus));
    writer.addSummaryRow("Peak Power (W)", maxOf(&DataRecord::power));

    if (!writer.save(path)) {
        emit statusChanged("Excel export failed: " + writer.lastError());
        return;
    }
    emit statusChanged("Excel saved → " + QFileInfo(path).fileName());
}

// ── Duration formatting ───────────────────────────────────────────────────
QString DeviceBackend::formatDuration(qint64 secs)
{
    return QString("%1:%2:%3")
        .arg(secs / 3600, 2, 10, QChar('0'))
        .arg((secs % 3600) / 60, 2, 10, QChar('0'))
        .arg(secs % 60, 2, 10, QChar('0'));
}
