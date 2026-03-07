#include "UsbTransport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QSocketNotifier>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QDebug>

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

// ── Protocol constants ────────────────────────────────────────────────────
static QByteArray makeCmd(uint8_t b1, uint8_t b2, uint8_t chk)
{
    QByteArray cmd(64, '\0');
    cmd[0] = static_cast<char>(0xAA);
    cmd[1] = static_cast<char>(b2);
    cmd[63] = static_cast<char>(chk);
    (void)b1; // aa prefix is always 0xAA above
    return cmd;
}

const QByteArray UsbTransport::CMD_INIT1 = [](){
    QByteArray c(64, '\0'); c[0]=char(0xAA); c[1]=char(0x81); c[63]=char(0x8E); return c; }();
const QByteArray UsbTransport::CMD_INIT2 = [](){
    QByteArray c(64, '\0'); c[0]=char(0xAA); c[1]=char(0x82); c[63]=char(0x96); return c; }();
const QByteArray UsbTransport::CMD_POLL  = [](){
    QByteArray c(64, '\0'); c[0]=char(0xAA); c[1]=char(0x83); c[63]=char(0x9E); return c; }();

// ── Constructor / destructor ──────────────────────────────────────────────
UsbTransport::UsbTransport(QObject* parent) : BaseTransport(parent) {}

void UsbTransport::requestStop()
{
    m_stop.store(true, std::memory_order_relaxed);
    quit(); // stop exec() event loop
}

// ── Sysfs discovery ───────────────────────────────────────────────────────
QString UsbTransport::findHidraw()
{
    const QString vid = "2e3c", pid = "5558";
    for (int attempt = 0; attempt < 15; ++attempt) {
        const auto entries = QDir("/sys/class/hidraw").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& name : entries) {
            const QString uevent = QString("/sys/class/hidraw/%1/device/../uevent").arg(name);
            QFile f(uevent);
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QString content = f.readAll();
            for (const auto& line : content.split('\n')) {
                if (line.startsWith("PRODUCT=") &&
                    line.mid(8).toLower().startsWith(vid + "/" + pid + "/"))
                    return "/dev/" + name;
            }
        }
        if (attempt < 14) {
            emit statusMessage("Waiting for FNB58 USB device…");
            QThread::sleep(1);
        }
    }
    return {};
}

// ── Write helper ──────────────────────────────────────────────────────────
bool UsbTransport::writeCmd(int fd, const QByteArray& cmd)
{
    for (int retry = 0; retry < 5; ++retry) {
        ssize_t n = ::write(fd, cmd.constData(), cmd.size());
        if (n == cmd.size()) return true;
        if (errno == ETIMEDOUT || errno == EPROTO) {
            QThread::msleep(150);
            continue;
        }
        return false;
    }
    return false;
}

// ── Device init ───────────────────────────────────────────────────────────
void UsbTransport::initDevice(int fd)
{
    // Drain stale data before sending init
    char drain[64];
    while (true) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        timeval tv{0, 20000}; // 20 ms
        if (::select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) break;
        if (::read(fd, drain, sizeof(drain)) <= 0) break;
    }

    writeCmd(fd, CMD_INIT1);
    QThread::msleep(50);
    writeCmd(fd, CMD_INIT2);
    QThread::msleep(50);
    writeCmd(fd, CMD_INIT2);
}

// ── Packet decoder ────────────────────────────────────────────────────────
void UsbTransport::decodeAndEmit(const char* buf)
{
    auto u8 = [&](int i){ return static_cast<uint8_t>(buf[i]); };
    if (u8(0) != 0xAA || u8(1) != 0x04) return;

    const int off = 2; // first sample starts at byte 2
    auto u32le = [&](int i) -> uint32_t {
        return u8(i) | (uint32_t(u8(i+1))<<8) | (uint32_t(u8(i+2))<<16) | (uint32_t(u8(i+3))<<24);
    };
    auto u16le = [&](int i) -> uint16_t {
        return u8(i) | (uint16_t(u8(i+1))<<8);
    };

    double vbus  = u32le(off + 0)  / 100000.0;
    double ibus  = u32le(off + 4)  / 100000.0;
    double dp    = u16le(off + 8)  / 1000.0;
    double dn    = u16le(off + 10) / 1000.0;
    double temp  = u16le(off + 13) / 10.0;
    emit reading(vbus, ibus, vbus * ibus, dp, dn, temp);
}

void UsbTransport::sendCommand(const QByteArray& cmd)
{
    if (m_fd >= 0)
        writeCmd(m_fd, cmd);
}

// ── Thread entry ─────────────────────────────────────────────────────────
void UsbTransport::run()
{
    const QString path = findHidraw();
    if (path.isEmpty()) {
        emit error("FNB58 USB device not found. Is it connected?");
        return;
    }

    int fd = ::open(path.toLocal8Bit().constData(), O_RDWR);
    if (fd < 0) {
        emit error(QString("Cannot open %1: %2").arg(path, strerror(errno)));
        return;
    }

    initDevice(fd);
    emit statusMessage("USB connected — streaming");
    m_fd = fd;

    // Try a gentle restart if device was already in measurement mode
    // (drain + INIT2 only first)
    auto tryPoll = [&]() -> bool {
        writeCmd(fd, CMD_POLL);
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        timeval tv{1, 0};
        return ::select(fd + 1, &rfds, nullptr, nullptr, &tv) > 0;
    };
    if (!tryPoll()) {
        initDevice(fd); // full init if no data yet
    }

    // QSocketNotifier notifies us when the HID fd has data
    auto* notifier = new QSocketNotifier(fd, QSocketNotifier::Read);
    // Poll timer keeps the firmware watchdog alive
    auto* pollTimer = new QTimer();
    pollTimer->setInterval(900); // every 0.9 s

    // Decimation: emit at most EMIT_INTERVAL_S
    QElapsedTimer emitTimer;
    emitTimer.start();

    connect(notifier, &QSocketNotifier::activated, [&]() {
        char buf[64] = {};
        if (::read(fd, buf, 64) == 64) {
            if (emitTimer.elapsed() >= static_cast<qint64>(EMIT_INTERVAL_S * 1000)) {
                decodeAndEmit(buf);
                emitTimer.restart();
            }
        }
    });

    connect(pollTimer, &QTimer::timeout, [&]() {
        writeCmd(fd, CMD_POLL);
    });

    // Stop checker
    auto* stopTimer = new QTimer();
    stopTimer->setInterval(100);
    connect(stopTimer, &QTimer::timeout, [this]() {
        if (m_stop.load(std::memory_order_relaxed))
            quit();
    });

    pollTimer->start();
    stopTimer->start();

    exec(); // run event loop

    m_fd = -1;
    delete notifier;
    delete pollTimer;
    delete stopTimer;
    ::close(fd);
}
