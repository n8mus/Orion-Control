// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio/SerialPort.h"

#include <QSocketNotifier>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace ttc {

SerialPort::SerialPort(QObject* parent) : QObject(parent) {}

SerialPort::~SerialPort() { close(); }

static speed_t toSpeed(int baud) {
    switch (baud) {
        case 9600:  return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200:return B115200;
        default:    return B57600;
    }
}

bool SerialPort::open(const std::string& device, int baud, bool hwHandshake) {
    close();
    fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        emit ioError(QStringLiteral("open %1: %2").arg(device.c_str(), std::strerror(errno)));
        return false;
    }

    termios tio{};
    if (tcgetattr(fd_, &tio) != 0) {
        emit ioError(QStringLiteral("tcgetattr: %1").arg(std::strerror(errno)));
        close();
        return false;
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, toSpeed(baud));
    cfsetospeed(&tio, toSpeed(baud));
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CSTOPB;                 // 1 stop bit
    tio.c_cflag &= ~PARENB;                 // no parity
    tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS8;  // 8 data bits
    if (hwHandshake) tio.c_cflag |= CRTSCTS;      // Omni VII needs RTS/CTS
    else             tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd_, TCSANOW, &tio) != 0) {
        emit ioError(QStringLiteral("tcsetattr: %1").arg(std::strerror(errno)));
        close();
        return false;
    }
    tcflush(fd_, TCIOFLUSH);

    notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, &SerialPort::onReadable);
    return true;
}

void SerialPort::close() {
    if (notifier_) { notifier_->setEnabled(false); notifier_->deleteLater(); notifier_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    rxBuf_.clear();
}

void SerialPort::write(const QByteArray& data) {
    if (fd_ < 0) return;
    ssize_t n = ::write(fd_, data.constData(), data.size());
    if (n < 0) emit ioError(QStringLiteral("write: %1").arg(std::strerror(errno)));
}

void SerialPort::onReadable() {
    char buf[512];
    for (;;) {
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) {
            rxBuf_.append(buf, static_cast<int>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0) { emit ioError(QStringLiteral("read: %1").arg(std::strerror(errno))); return; }
        break; // n == 0
    }
    // Ten-Tec responses are terminated by CR (0x0D).
    int idx;
    while ((idx = rxBuf_.indexOf('\r')) >= 0) {
        QByteArray line = rxBuf_.left(idx);
        rxBuf_.remove(0, idx + 1);
        if (!line.isEmpty()) emit lineReceived(line);
    }
}

} // namespace ttc
