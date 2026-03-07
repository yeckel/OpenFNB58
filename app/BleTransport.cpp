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
    QLowEnergyService* svc = nullptr;

    connect(ctl, &QLowEnergyController::connected, [&]() {
        emit statusMessage(
            QString("BLE connected to %1, discovering services…")
            .arg(m_device.address().toString()));
        ctl->discoverServices();
    });

    connect(ctl, &QLowEnergyController::discoveryFinished, [&]() {
        svc = ctl->createServiceObject(
            QBluetoothUuid(QLatin1String(UUID_SERVICE)));
        if (!svc) {
            emit error("FFE0 service not found — is this an FNB58?");
            loop.quit();
            return;
        }

        connect(svc, &QLowEnergyService::stateChanged,
                [&, svc](QLowEnergyService::ServiceState state) {
            if (state != QLowEnergyService::RemoteServiceDiscovered) return;

            auto notifyChar = svc->characteristic(
                QBluetoothUuid(QLatin1String(UUID_NOTIFY)));
            auto writeChar  = svc->characteristic(
                QBluetoothUuid(QLatin1String(UUID_WRITE)));

            if (!notifyChar.isValid() || !writeChar.isValid()) {
                emit error("FFE4/FFE9 characteristics not found");
                loop.quit();
                return;
            }

            // Enable notifications via Client Characteristic Configuration Descriptor
            auto cccd = notifyChar.descriptor(
                QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
            if (cccd.isValid())
                svc->writeDescriptor(cccd, QByteArray::fromHex("0100"));

            // Expose write char for sendCommand()
            {
                QMutexLocker lk(&m_writeMutex);
                m_svc       = svc;
                m_writeChar = writeChar;
            }

            svc->writeCharacteristic(writeChar, BLE_INIT1,
                                     QLowEnergyService::WriteWithoutResponse);
            QTimer::singleShot(2000, [this, svc, writeChar]() mutable {
                if (!m_stop)
                    svc->writeCharacteristic(writeChar, BLE_INIT2,
                                             QLowEnergyService::WriteWithoutResponse);
            });

            emit statusMessage(
                QString("BLE streaming — %1").arg(m_device.address().toString()));
        });

        connect(svc, &QLowEnergyService::characteristicChanged,
                [&](const QLowEnergyCharacteristic& c, const QByteArray& value) {
            if (c.uuid() == QBluetoothUuid(QLatin1String(UUID_NOTIFY)))
                parseBlePacket(value);
        });

        connect(svc, &QLowEnergyService::errorOccurred,
                [&](QLowEnergyService::ServiceError e) {
            emit error(QString("BLE service error %1").arg(static_cast<int>(e)));
            loop.quit();
        });

        svc->discoverDetails();
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
    if (svc) delete svc;
    ctl->disconnectFromDevice();
    delete ctl;
}

#endif // FNB58_HAVE_BLUETOOTH
