#pragma once
#include "BaseTransport.h"
#include <QString>
#include <QByteArray>
#include <QMutex>
#include <atomic>

#ifdef FNB58_HAVE_BLUETOOTH

#include <QBluetoothDeviceInfo>
#include <QLowEnergyCharacteristic>

class QLowEnergyService;

class BleTransport : public BaseTransport
{
    Q_OBJECT
public:
    explicit BleTransport(const QBluetoothDeviceInfo& device,
                          QObject* parent = nullptr);
    void requestStop() override;
    void sendCommand(const QByteArray& cmd) override;

protected:
    void run() override;

private:
    QBluetoothDeviceInfo m_device;
    std::atomic<bool>    m_stop{false};

    static constexpr auto UUID_SERVICE = "0000ffe0-0000-1000-8000-00805f9b34fb";
    static constexpr auto UUID_NOTIFY  = "0000ffe4-0000-1000-8000-00805f9b34fb";
    static constexpr auto UUID_WRITE   = "0000ffe9-0000-1000-8000-00805f9b34fb";

    static const QByteArray BLE_INIT1;
    static const QByteArray BLE_INIT2;

    static constexpr int PKT_LENS[9] = {0,0,0,14,12,7,6,4,17};

    QMutex                   m_writeMutex;
    QLowEnergyService*       m_svc{nullptr};
    QLowEnergyCharacteristic m_writeChar;

    double m_lastDp = 0, m_lastDn = 0;
    bool   m_hasDp  = false;

    void parseBlePacket(const QByteArray& data);
};

#else  // !FNB58_HAVE_BLUETOOTH

// Stub so DeviceBackend compiles without Qt Bluetooth
class QBluetoothDeviceInfo;  // forward only

class BleTransport : public BaseTransport
{
    Q_OBJECT
public:
    explicit BleTransport(const QString& /*mac*/, QObject* parent = nullptr)
        : BaseTransport(parent) {}
    void requestStop() override {}

protected:
    void run() override {
        emit error("BLE transport not available: Qt Bluetooth not installed.");
    }
};

#endif // FNB58_HAVE_BLUETOOTH
