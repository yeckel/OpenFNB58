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
    void sendCommand(const QByteArray& cmd) override;

protected:
    void run() override;

private:
    std::atomic<bool> m_stop{false};
    int m_fd = -1;  // Set during run(); safe to access in transport thread

    static const QByteArray CMD_INIT1;
    static const QByteArray CMD_INIT2;
    static const QByteArray CMD_POLL;

    static constexpr double EMIT_INTERVAL_S = 0.1;

    QString findHidraw();
    bool    writeCmd(int fd, const QByteArray& cmd);
    void    initDevice(int fd);
    void    decodeAndEmit(const char* buf);
};
