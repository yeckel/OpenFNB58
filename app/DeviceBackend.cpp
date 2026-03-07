#include "DeviceBackend.h"
#include "UsbTransport.h"
#include "BleTransport.h"
#include "XlsxWriter.h"

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QFileInfo>
#include <QUrl>
#include <QProcess>
#include <QtMath>
#include <cmath>
#include <limits>

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
        m_transport = new BleTransport(address.trimmed(), this);
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
    emit statusChanged("Scanning for paired BLE devices…");
    QProcess proc;
    proc.start("bluetoothctl", {"devices", "Paired"});
    proc.waitForFinished(5000);
    const QString out = proc.readAllStandardOutput();

    QStringList result;
    for (const QString& line : out.split('\n', Qt::SkipEmptyParts)) {
        // Format: "Device BA:03:18:7A:23:DF FNB58-038059"
        const QStringList parts = line.trimmed().split(' ', Qt::SkipEmptyParts);
        if (parts.size() >= 3 && parts[0] == "Device") {
            const QString mac  = parts[1];
            const QString name = parts.mid(2).join(' ');
            result << QString("%1 (%2)").arg(name, mac);
        }
    }

    if (result.isEmpty())
        emit statusChanged("No paired BLE devices found. Pair first with bluetoothctl.");
    else
        emit statusChanged(QString("Found %1 paired device(s)").arg(result.size()));

    emit bleDevicesFound(result);
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
