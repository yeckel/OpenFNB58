#pragma once
#include "BaseTransport.h"
#include <QString>
#include <QByteArray>
#include <QVariantMap>
#include <QStringList>
#include <atomic>

class BleTransport : public BaseTransport
{
    Q_OBJECT
public:
    explicit BleTransport(const QString& mac, QObject* parent = nullptr);
    void requestStop() override;

protected:
    void run() override;

private slots:
    void onPropertiesChanged(const QString& iface,
                             const QVariantMap& changed,
                             const QStringList& invalidated);

private:
    QString m_mac;
    std::atomic<bool> m_stop{false};

    static constexpr auto UUID_NOTIFY = "0000ffe4-0000-1000-8000-00805f9b34fb";
    static constexpr auto UUID_WRITE  = "0000ffe9-0000-1000-8000-00805f9b34fb";

    static const QByteArray BLE_INIT1;
    static const QByteArray BLE_INIT2;

    // packet data_len by type (index = type byte)
    static constexpr int PKT_LENS[9] = {0,0,0,14,12,7,6,4,17};

    bool ensureConnected();
    bool findGattChars(QString& notifyPath, QString& writePath);
    void writeGatt(const QString& path, const QByteArray& data);

    // Parser state shared between thread and slot (both run in the thread)
    double m_lastDp = 0, m_lastDn = 0;
    bool   m_hasDp  = false;
};
