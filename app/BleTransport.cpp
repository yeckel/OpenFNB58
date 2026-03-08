#include "BleTransport.h"

#ifdef FNB58_HAVE_BLUETOOTH

#include <QBluetoothAddress>
#include <QBluetoothUuid>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QEventLoop>
#include <QTimer>
#include <QMutexLocker>
#include <QDebug>

#include <limits>
#include <cstdint>

// ── Static constants ───────────────────────────────────────────────────────
const QByteArray BleTransport::BLE_INIT1 = QByteArray::fromHex("aa8100f4");
const QByteArray BleTransport::BLE_INIT2 = QByteArray::fromHex("aa8200a7");
constexpr int    BleTransport::PKT_LENS[9];

BleTransport::BleTransport(const QBluetoothDeviceInfo& device, QObject* parent)
    : BaseTransport(parent), m_device(device)
{}

void BleTransport::requestStop()
{
    m_stop.store(true, std::memory_order_relaxed);
}

void BleTransport::sendCommand(const QByteArray& cmd)
{
    QMutexLocker lock(&m_writeMutex);
    if (m_svc && m_writeChar.isValid())
        m_svc->writeCharacteristic(m_writeChar, cmd,
                                   QLowEnergyService::WriteWithoutResponse);
}

// ── BLE packet parser (unchanged protocol) ────────────────────────────────
void BleTransport::parseBlePacket(const QByteArray& chunk)
{
    const auto* d   = reinterpret_cast<const uint8_t*>(chunk.constData());
    int         i   = 0;
    const int   len = chunk.size();
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

        if (ptype == 0x07) {
            double vbus = u16(p);
            double ibus = u16(p + 2);
            emit reading(vbus, ibus, vbus * ibus,
                         m_hasDp ? m_lastDp : nan,
                         m_hasDp ? m_lastDn : nan,
                         nan);
        } else if (ptype == 0x06) {
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
    auto* ctl = QLowEnergyController::createCentral(m_device);
    QEventLoop loop;

    // FFE4 (notify) is in service FFE0; FFE9 (write) is in service FFE5
    QLowEnergyService* svcNotify = nullptr;
    QLowEnergyService* svcWrite  = nullptr;
    int discoveredCount = 0;   // tracks how many of the two services are ready

    auto tryStart = [&]() {
        if (discoveredCount < 2) return;

        auto notifyChar = svcNotify->characteristic(
            QBluetoothUuid(QLatin1String(UUID_NOTIFY)));
        auto writeChar  = svcWrite->characteristic(
            QBluetoothUuid(QLatin1String(UUID_WRITE)));

        if (!notifyChar.isValid() || !writeChar.isValid()) {
            // Log what we actually found to help future debugging
            qWarning() << "FFE4/FFE9 not found. FFE0 chars:";
            for (auto& c : svcNotify->characteristics())
                qWarning() << " " << c.uuid().toString();
            qWarning() << "FFE5 chars:";
            for (auto& c : svcWrite->characteristics())
                qWarning() << " " << c.uuid().toString();
            emit error("FFE4/FFE9 characteristics not found");
            loop.quit();
            return;
        }

        // Enable notifications via CCCD
        auto cccd = notifyChar.descriptor(
            QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
        if (cccd.isValid())
            svcNotify->writeDescriptor(cccd, QByteArray::fromHex("0100"));

        {
            QMutexLocker lk(&m_writeMutex);
            m_svc       = svcWrite;
            m_writeChar = writeChar;
        }

        svcWrite->writeCharacteristic(writeChar, BLE_INIT1,
                                      QLowEnergyService::WriteWithoutResponse);
        QTimer::singleShot(2000, [this, svcWrite, writeChar]() mutable {
            if (!m_stop)
                svcWrite->writeCharacteristic(writeChar, BLE_INIT2,
                                              QLowEnergyService::WriteWithoutResponse);
        });

        emit statusMessage(
            QString("BLE streaming — %1").arg(m_device.address().toString()));
    };

    connect(ctl, &QLowEnergyController::connected, [&]() {
        emit statusMessage(
            QString("BLE connected to %1, discovering services…")
            .arg(m_device.address().toString()));
        ctl->discoverServices();
    });

    connect(ctl, &QLowEnergyController::discoveryFinished, [&]() {
        svcNotify = ctl->createServiceObject(
            QBluetoothUuid(QLatin1String(UUID_SVC_NOTIFY)));
        svcWrite  = ctl->createServiceObject(
            QBluetoothUuid(QLatin1String(UUID_SVC_WRITE)));

        if (!svcNotify || !svcWrite) {
            emit error(QString("Required BLE services not found (FFE0=%1, FFE5=%2)")
                       .arg(svcNotify ? "ok" : "missing")
                       .arg(svcWrite  ? "ok" : "missing"));
            loop.quit();
            return;
        }

        auto onState = [&](QLowEnergyService::ServiceState state) {
            if (state == QLowEnergyService::RemoteServiceDiscovered) {
                ++discoveredCount;
                tryStart();
            } else if (state == QLowEnergyService::InvalidService) {
                emit error("BLE service detail discovery failed");
                loop.quit();
            }
        };

        connect(svcNotify, &QLowEnergyService::stateChanged, onState);
        connect(svcWrite,  &QLowEnergyService::stateChanged, onState);

        connect(svcNotify, &QLowEnergyService::characteristicChanged,
                [&](const QLowEnergyCharacteristic& c, const QByteArray& value) {
            if (c.uuid() == QBluetoothUuid(QLatin1String(UUID_NOTIFY)))
                parseBlePacket(value);
        });

        connect(svcNotify, &QLowEnergyService::errorOccurred,
                [&](QLowEnergyService::ServiceError e) {
            emit error(QString("BLE notify service error %1").arg(static_cast<int>(e)));
            loop.quit();
        });
        connect(svcWrite, &QLowEnergyService::errorOccurred,
                [&](QLowEnergyService::ServiceError e) {
            emit error(QString("BLE write service error %1").arg(static_cast<int>(e)));
            loop.quit();
        });

        svcNotify->discoverDetails();
        svcWrite->discoverDetails();
    });

    connect(ctl, &QLowEnergyController::errorOccurred,
            [&](QLowEnergyController::Error) {
        emit error(QString("BLE error: %1").arg(ctl->errorString()));
        loop.quit();
    });

    connect(ctl, &QLowEnergyController::disconnected, [&]() {
        if (!m_stop) emit error("BLE device disconnected unexpectedly");
        loop.quit();
    });

    // Poll m_stop every 100 ms so requestStop() is honored promptly
    auto* stopTimer = new QTimer();
    stopTimer->setInterval(100);
    QObject::connect(stopTimer, &QTimer::timeout, [&]() {
        if (m_stop.load()) loop.quit();
    });
    stopTimer->start();

    emit statusMessage(
        QString("Connecting to BLE %1…").arg(m_device.address().toString()));
    ctl->connectToDevice();
    loop.exec();

    // ── Cleanup ────────────────────────────────────────────────────────────
    {
        QMutexLocker lk(&m_writeMutex);
        m_svc = nullptr;
        m_writeChar = QLowEnergyCharacteristic{};
    }
    stopTimer->stop();
    delete stopTimer;
    if (svcNotify) delete svcNotify;
    if (svcWrite && svcWrite != svcNotify) delete svcWrite;
    ctl->disconnectFromDevice();
    delete ctl;
}

#endif // FNB58_HAVE_BLUETOOTH
