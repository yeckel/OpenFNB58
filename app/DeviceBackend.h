#pragma once
#include "DataRecord.h"
#include "BaseTransport.h"

#include <QObject>
#include <QVector>
#include <QTimer>
#include <QString>

class DeviceBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    running     READ running     NOTIFY runningChanged)
    Q_PROPERTY(int     sampleCount READ sampleCount NOTIFY sampleCountChanged)
    Q_PROPERTY(double  energyWh    READ energyWh    NOTIFY energyChanged)
    Q_PROPERTY(QString duration    READ duration    NOTIFY durationChanged)

public:
    explicit DeviceBackend(QObject* parent = nullptr);
    ~DeviceBackend() override;

    bool   running()     const { return m_running; }
    int    sampleCount() const { return m_readings.size(); }
    double energyWh()    const { return m_energyWh - m_energyBase; }
    QString duration()   const { return m_duration; }

public slots:
    void start(const QString& transport, const QString& address);
    void stop();
    void exportCsv(const QString& path);
    void exportExcel(const QString& path);
    void resetEnergy();
    void scanBleDevices();
    Q_INVOKABLE QVariantMap measureRange(double tStart, double tEnd) const;

signals:
    // Emitted for every new reading; t is seconds since session start
    void newReading(double t, double vbus, double ibus, double power,
                    double dp, double dn, double temp);
    void statusChanged(const QString& msg);
    void runningChanged(bool running);
    void sampleCountChanged(int count);
    void errorOccurred(const QString& msg);
    void energyChanged(double wh);
    void durationChanged(const QString& hms);
    // emits list of "Name (MAC)" strings for BLE device picker
    void bleDevicesFound(QStringList devices);

private slots:
    void onReading(double vbus, double ibus, double power,
                   double dp, double dn, double temp);
    void onTransportError(const QString& msg);
    void onStatusMessage(const QString& msg);
    void onDurationTick();

private:
    void cleanupTransport();
    static QString formatDuration(qint64 secs);

    bool          m_running   = false;
    QVector<DataRecord> m_readings;
    double        m_energyWh  = 0.0;
    double        m_energyBase= 0.0; // for resetEnergy()
    double        m_startTime = 0.0;
    double        m_lastT     = -1.0;
    double        m_lastPower = 0.0;
    QString       m_duration  = "00:00:00";

    BaseTransport* m_transport    = nullptr;
    QTimer*        m_durationTimer= nullptr;
};
