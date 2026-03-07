#pragma once
#include <QThread>
#include <QString>

// Abstract base class for USB and BLE transport threads.
// Both run in their own QThread via exec() so Qt timers and
// QSocketNotifier / QDBus signal delivery work correctly.
class BaseTransport : public QThread
{
    Q_OBJECT
public:
    explicit BaseTransport(QObject* parent = nullptr) : QThread(parent) {}

    // Thread-safe; may be called from any thread.
    virtual void requestStop() = 0;

public slots:
    // Invoked in transport thread (Qt::QueuedConnection) to write raw command.
    virtual void sendCommand(const QByteArray& /*cmd*/) {}

signals:
    // vbus/ibus/power in V/A/W; dp/dn/temp may be NaN for BLE
    void reading(double vbus, double ibus, double power,
                 double dp,   double dn,   double temp);
    void error(const QString& msg);
    void statusMessage(const QString& msg);
};
