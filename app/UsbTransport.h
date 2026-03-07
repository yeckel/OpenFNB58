#pragma once
#include "BaseTransport.h"
#include <QTimer>
#include <QSocketNotifier>
#include <atomic>

class UsbTransport : public BaseTransport
{
    Q_OBJECT
public:
    explicit UsbTransport(QObject* parent = nullptr);
    void requestStop() override;

protected:
    void run() override;

private:
    std::atomic<bool> m_stop{false};

    // Commands (64-byte HID reports)
    static const QByteArray CMD_INIT1;
    static const QByteArray CMD_INIT2;
    static const QByteArray CMD_POLL;

    static constexpr double EMIT_INTERVAL_S = 0.1; // 10 Hz to QML

    QString findHidraw();
    bool    writeCmd(int fd, const QByteArray& cmd);
    void    initDevice(int fd);
    void    decodeAndEmit(const char* buf);
};
