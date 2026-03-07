#pragma once
#include "DataRecord.h"
#include "BaseTransport.h"

#include <QObject>
#include <QVector>
#include <QTimer>
#include <QString>
#include <QVariantMap>

#ifdef FNB58_HAVE_BLUETOOTH
#  include <QMap>
#  include <QBluetoothDeviceInfo>
#endif

class DeviceBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    running      READ running      NOTIFY runningChanged)
    Q_PROPERTY(int     sampleCount  READ sampleCount  NOTIFY sampleCountChanged)
    Q_PROPERTY(double  energyWh     READ energyWh     NOTIFY energyChanged)
    Q_PROPERTY(QString duration     READ duration     NOTIFY durationChanged)
    Q_PROPERTY(QString protocolName READ protocolName NOTIFY protocolChanged)

public:
    explicit DeviceBackend(QObject* parent = nullptr);
    ~DeviceBackend() override;

    bool    running()      const { return m_running; }
    int     sampleCount()  const { return m_readings.size(); }
    double  energyWh()     const { return m_energyWh - m_energyBase; }
    QString duration()     const { return m_duration; }
    QString protocolName() const { return m_protocolName; }

    // Protocol table: id → {name, defaultVoltage_mV, {available voltages_mV}}
    struct ProtocolInfo {
        int     id;
        QString name;
        int     defaultVoltage_mV;
        QList<int> voltages_mV;
    };
    static QList<ProtocolInfo> allProtocols();
    static QString inferProtocol(double dp, double dn, double vbus);

public slots:
    void start(const QString& transport, const QString& address);
    void stop();
    void exportCsv(const QString& path);
    void exportExcel(const QString& path);
    void resetEnergy();
    void scanBleDevices();
    void sendTrigger(int protoId, int voltage_mV);
    void releaseTrigger();
    Q_INVOKABLE QVariantMap measureRange(double tStart, double tEnd) const;

signals:
    void newReading(double t, double vbus, double ibus, double power,
                    double dp, double dn, double temp);
    void statusChanged(const QString& msg);
    void runningChanged(bool running);
    void sampleCountChanged(int count);
    void errorOccurred(const QString& msg);
    void energyChanged(double wh);
    void durationChanged(const QString& hms);
    void bleDevicesFound(QStringList devices);
    void protocolChanged(const QString& name);

private slots:
    void onReading(double vbus, double ibus, double power,
                   double dp, double dn, double temp);
    void onTransportError(const QString& msg);
    void onStatusMessage(const QString& msg);
    void onDurationTick();

private:
    void cleanupTransport();
    static QString formatDuration(qint64 secs);

    bool          m_running    = false;
    QVector<DataRecord> m_readings;
    double        m_energyWh   = 0.0;
    double        m_energyBase = 0.0;
    double        m_startTime  = 0.0;
    double        m_lastT      = -1.0;
    double        m_lastPower  = 0.0;
    QString       m_duration   = "00:00:00";
    QString       m_protocolName;

    BaseTransport* m_transport     = nullptr;
    QTimer*        m_durationTimer = nullptr;
#ifdef FNB58_HAVE_BLUETOOTH
    QMap<QString, QBluetoothDeviceInfo> m_bleDeviceCache;
#endif
};
