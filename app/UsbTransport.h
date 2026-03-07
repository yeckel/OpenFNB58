#pragma once
#include "BaseTransport.h"
#include <QMutex>
#include <QElapsedTimer>
#include <atomic>

struct hid_device_;  // forward declaration (avoids pulling in hidapi.h here)

class UsbTransport : public BaseTransport
{
    Q_OBJECT
public:
    explicit UsbTransport(QObject* parent = nullptr);
    void requestStop() override;
    void sendCommand(const QByteArray& cmd) override;

protected:
    void run() override;

private:
    std::atomic<bool> m_stop{false};
    hid_device_*      m_dev{nullptr};   // set inside run(); protected by m_devMutex
    QMutex            m_devMutex;

    static const QByteArray CMD_INIT1;
    static const QByteArray CMD_INIT2;
    static const QByteArray CMD_POLL;

    static constexpr double   EMIT_INTERVAL_S = 0.1;
    static constexpr uint16_t FNB58_VID = 0x2e3c;
    static constexpr uint16_t FNB58_PID = 0x5558;

    bool sendHidCmd(hid_device_* dev, const QByteArray& cmd);
    void decodeAndEmit(const uint8_t* buf);
};
