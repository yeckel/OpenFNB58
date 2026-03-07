#include "UsbTransport.h"

#include <hidapi/hidapi.h>
#include <QThread>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QDebug>

#include <cstring>
#include <cstdint>

// ── Protocol constants ────────────────────────────────────────────────────
const QByteArray UsbTransport::CMD_INIT1 = [](){
    QByteArray c(64, '\0'); c[0]=char(0xAA); c[1]=char(0x81); c[63]=char(0x8E); return c; }();
const QByteArray UsbTransport::CMD_INIT2 = [](){
    QByteArray c(64, '\0'); c[0]=char(0xAA); c[1]=char(0x82); c[63]=char(0x96); return c; }();
const QByteArray UsbTransport::CMD_POLL  = [](){
    QByteArray c(64, '\0'); c[0]=char(0xAA); c[1]=char(0x83); c[63]=char(0x9E); return c; }();

UsbTransport::UsbTransport(QObject* parent) : BaseTransport(parent) {}

void UsbTransport::requestStop()
{
    m_stop.store(true, std::memory_order_relaxed);
}

// ── Write helper (prepends 0x00 report-ID required by hidapi) ─────────────
bool UsbTransport::sendHidCmd(hid_device_* dev, const QByteArray& cmd)
{
    uint8_t buf[65] = {};
    buf[0] = 0x00;  // report ID (none — device uses single report)
    std::memcpy(buf + 1, cmd.constData(), qMin(cmd.size(), 64));
    for (int retry = 0; retry < 5; ++retry) {
        if (hid_write(dev, buf, 65) > 0) return true;
        QThread::msleep(150);
    }
    return false;
}

// ── Packet decoder ────────────────────────────────────────────────────────
void UsbTransport::decodeAndEmit(const uint8_t* buf)
{
    if (buf[0] != 0xAA || buf[1] != 0x04) return;

    const int off = 2;
    auto u32le = [&](int i) -> uint32_t {
        return buf[i] | (uint32_t(buf[i+1])<<8) | (uint32_t(buf[i+2])<<16) | (uint32_t(buf[i+3])<<24);
    };
    auto u16le = [&](int i) -> uint16_t {
        return buf[i] | (uint16_t(buf[i+1])<<8);
    };

    double vbus = u32le(off + 0)  / 100000.0;
    double ibus = u32le(off + 4)  / 100000.0;
    double dp   = u16le(off + 8)  / 1000.0;
    double dn   = u16le(off + 10) / 1000.0;
    double temp = u16le(off + 13) / 10.0;
    emit reading(vbus, ibus, vbus * ibus, dp, dn, temp);
}

// ── Thread-safe command injection (called from main thread for triggers) ──
void UsbTransport::sendCommand(const QByteArray& cmd)
{
    QMutexLocker lock(&m_devMutex);
    if (m_dev) sendHidCmd(m_dev, cmd);
}

// ── Thread entry ─────────────────────────────────────────────────────────
void UsbTransport::run()
{
    hid_init();

    hid_device_* dev = nullptr;
    emit statusMessage("Waiting for FNB58 USB device…");
    for (int i = 0; i < 150 && !m_stop; ++i) {
        dev = hid_open(FNB58_VID, FNB58_PID, nullptr);
        if (dev) break;
        QThread::msleep(100);
    }
    if (!dev) {
        emit error("FNB58 USB device not found (VID=2e3c PID=5558). "
                   "Is it connected?");
        hid_exit();
        return;
    }

    // Drain stale data (non-blocking reads until empty)
    hid_set_nonblocking(dev, 1);
    uint8_t drain[64];
    while (hid_read(dev, drain, sizeof(drain)) > 0) {}
    hid_set_nonblocking(dev, 0);

    // Init sequence
    sendHidCmd(dev, CMD_INIT1);
    QThread::msleep(50);
    sendHidCmd(dev, CMD_INIT2);
    QThread::msleep(50);
    sendHidCmd(dev, CMD_INIT2);

    {
        QMutexLocker lock(&m_devMutex);
        m_dev = dev;
    }
    emit statusMessage("USB connected — streaming");

    QElapsedTimer emitTimer, pollTimer;
    emitTimer.start();
    pollTimer.start();

    uint8_t rbuf[64];
    while (!m_stop) {
        // hid_read_timeout: blocks up to 200 ms, returns 0 on timeout
        int n = hid_read_timeout(dev, rbuf, sizeof(rbuf), 200);
        if (n < 0) {
            emit error("USB HID read error — device disconnected?");
            break;
        }

        if (pollTimer.elapsed() >= 900) {
            sendHidCmd(dev, CMD_POLL);
            pollTimer.restart();
        }

        if (n > 0 && emitTimer.elapsed() >= static_cast<qint64>(EMIT_INTERVAL_S * 1000)) {
            decodeAndEmit(rbuf);
            emitTimer.restart();
        }
    }

    {
        QMutexLocker lock(&m_devMutex);
        m_dev = nullptr;
    }
    hid_close(dev);
    hid_exit();
}
