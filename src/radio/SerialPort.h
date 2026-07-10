// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QByteArray>
#include <string>

class QSocketNotifier;

namespace ttc {

// Minimal POSIX serial wrapper integrated into the Qt event loop via a
// QSocketNotifier. Line-oriented (CR-terminated) to match the Ten-Tec ASCII
// protocol. Portable choice later: qt6-serialport or libserialport; kept
// dependency-free here so the skeleton builds on a bare Qt6 install.
class SerialPort : public QObject {
    Q_OBJECT
public:
    explicit SerialPort(QObject* parent = nullptr);
    ~SerialPort() override;

    // 57600 8N1 for both Ten-Tec radios; hwHandshake=true for the Omni VII.
    bool open(const std::string& device, int baud = 57600, bool hwHandshake = false);
    void close();
    bool isOpen() const { return fd_ >= 0; }

    void write(const QByteArray& data);

signals:
    void lineReceived(const QByteArray& line);   // one CR-delimited response
    void ioError(const QString& what);

private slots:
    void onReadable();

private:
    int fd_ = -1;
    QSocketNotifier* notifier_ = nullptr;
    QByteArray rxBuf_;
};

} // namespace ttc
