/*****************************************************************************
  The MIT License

  Copyright © 2021 Pavel Karelin (hkarel), <hkarel@yandex.ru>

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*****************************************************************************/

#include "usb_relay.h"

#include "shared/utils.h"
#include "shared/logger/logger.h"
#include "shared/logger/format.h"
#include "shared/qt/logger_operators.h"

#include <stdlib.h>
#include <string.h>

#define log_error_m   alog::logger().error   (alog_line_location, "UsbRelay")
#define log_warn_m    alog::logger().warn    (alog_line_location, "UsbRelay")
#define log_info_m    alog::logger().info    (alog_line_location, "UsbRelay")
#define log_verbose_m alog::logger().verbose (alog_line_location, "UsbRelay")
#define log_debug_m   alog::logger().debug   (alog_line_location, "UsbRelay")
#define log_debug2_m  alog::logger().debug2  (alog_line_location, "UsbRelay")

#define USB_RELAY_VENDOR_ID      0x16c0
#define USB_RELAY_DEVICE_ID      0x05df

#define USBRQ_HID_GET_REPORT     0x01
#define USBRQ_HID_SET_REPORT     0x09
#define REPORT_REQUEST_TIMEOUT   2*1000  // 2 секунды
#define USB_CONTINUOUS_ERRORS_1  3
#define USB_CONTINUOUS_ERRORS_2  5

namespace usb {

// Допустимый диапазон имен  [USBRelay1...USBRelay8]
static const char* baseProductName = "USBRelay";

bool Relay::init(const QVector<int>& states)
{
    QMutexLocker locker {&_threadLock}; (void) locker;

    _initStates = states;
    usb_init();
    return true;
}

QString Relay::product() const
{
    QMutexLocker locker {&_threadLock}; (void) locker;
    return _product;
}

QString Relay::serial() const
{
    QMutexLocker locker {&_threadLock}; (void) locker;
    return _serial;
}

bool Relay::setSerial(const QString& value)
{
    const int serialLen = 5;
    QByteArray val = value.toUtf8();

    if (val.length() > serialLen)
        val.resize(serialLen);

    while (val.length() < serialLen)
        val.append('0');

    for (int i = 0; i < serialLen; ++i)
    {
        uchar ch = uchar(val[i]);
        if ((ch <= 0x20) || (ch >= 0x7F))
        {
            log_error_m << log_format(
                "Incorrect USB relay serial. Symbol index: %?; code: %?",
                i, int(ch));
            return false;
        }
    }

    char buff[8] = {0};
    int  buffSize = sizeof(buff);

    buff[0] = 0xFA; // CMD_SET_SERIAL;
    for (int i = 1; i <= serialLen; ++i)
        buff[i] = val[i - 1];

    int res = usb_control_msg(_deviceHandle,
                              USB_TYPE_CLASS | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
                              USBRQ_HID_SET_REPORT,
                              0, // value
                              0, // index
                              buff, buffSize,
                              REPORT_REQUEST_TIMEOUT);
    if (res != buffSize)
    {
        alog::Line logLine =
            log_error_m << "Failed set USB relay serial: " << val;
        if (res < 0)
        {
            _usbLastErrorCode = res;
            logLine << ". Error code: " << res << "; " << usb_strerror();
        }
        ++_usbContinuousErrors;
        return false;
    }

    memset(buff, 0, buffSize);
    res = readStates(buff, buffSize);
    if (res < 0)
    {
        log_error_m << "Failed get USB relay serial";
        return false;
    }
    if (buff[serialLen + 1] != 0)
    {
        log_error_m << "Bad USB relay serial string";
        return false;
    }

    QString serial = QString::fromLatin1(buff);
    log_verbose_m << "USB relay new serial: " << serial;

    QMutexLocker locker {&_threadLock}; (void) locker;

    _serial = serial;
    return true;
}

int Relay::count() const
{
    QMutexLocker locker {&_threadLock}; (void) locker;
    return _count;
}

void Relay::run()
{
    log_info_m << "Started";

    quint32 captureAttempts = 0;

    while (true)
    {
        CHECK_QTHREADEX_STOP

        _deviceInitialized = false;
        if (captureDevice())
            _deviceInitialized = true;

        if (!_deviceInitialized)
        {
            releaseDevice();
            int timeout = 2;
            if (captureAttempts > 40)
                timeout = 15;
            else if (captureAttempts > 20)
                timeout = 10;
            ++captureAttempts;
            sleep(timeout);
            continue;
        }
        captureAttempts = 0;

        log_info_m << "USB relay emit signal 'attached'";
        emit attached();

        { //Block for QMutexLocker
            QMutexLocker locker(&_threadLock); (void) locker;
            if (!_initStates.isEmpty())
            {
                if (_initStates.count() > _count)
                    _initStates.resize(_count);

                QVector<int> states = statesInternal();
                for (int i = 0; i < _count; ++i)
                    if (_initStates[i] != states[i])
                        toggleInternal(i + 1, _initStates[i]);

                _initStates.clear();

                QVariant vstat;
                vstat.setValue(statesInternal());
                log_verbose_m << "USB init relay states: " << vstat;
            }
        }

        while (true)
        {
            CHECK_QTHREADEX_STOP

            if (_usbContinuousErrors >= USB_CONTINUOUS_ERRORS_1
                && _usbLastErrorCode == -19 /*Ошибка: 'Нет устройства'*/)
                break;

            if (_usbContinuousErrors >= USB_CONTINUOUS_ERRORS_2)
                break;

            { //Block for QMutexLocker
                QMutexLocker locker(&_threadLock); (void) locker;
                _threadCond.wait(&_threadLock, 200);
            }
            if (threadStop())
                continue;

            { //Block for QMutexLocker
                QMutexLocker locker {&_threadLock}; (void) locker;
                char buff[8] = {0};
                int res = readStates(buff, sizeof(buff));
                if (res < 0)
                    continue;

                quint8 states = quint8(res);
                if (_states != states)
                {
                    log_debug_m << log_format(
                        "USB relay state was changed from outside"
                        ". Old value: %?. New value: %?", int(_states), int(states));
                    _states = states;
                }
            }
        } // while (true)

        log_info_m << "USB relay emit signal 'detached'";
        emit detached();
        releaseDevice();

    } // while (true)

    releaseDevice();

    log_info_m << "Stopped";
}

void Relay::threadStopEstablished()
{
    QMutexLocker locker {&_threadLock}; (void) locker;
    _threadCond.wakeAll();
}

bool Relay::captureDevice()
{
    struct usb_bus* bus;
    struct usb_device* device;

    usb_find_busses();
    usb_find_devices();

    #define USB_CLOSE { \
        usb_close(_deviceHandle); \
        _deviceHandle = nullptr; \
        log_verbose_m << "USB interface closed"; }

    for (bus = usb_get_busses(); bus; bus = bus->next)
    {
        CHECK_QTHREADEX_STOP

        for (device = bus->devices; device; device = device->next)
        {
            CHECK_QTHREADEX_STOP

            if (device->descriptor.idVendor == USB_RELAY_VENDOR_ID
                && device->descriptor.idProduct == USB_RELAY_DEVICE_ID)
            {
                sscanf(bus->dirname, "%d", &(_usbBusNumber));
                _usbDeviceNumber = device->devnum;

                log_info_m << "USB relay device found on bus "
                           << utl::formatMessage("%03d/%03d", _usbBusNumber, _usbDeviceNumber);

                int attempts = 1;
                while (true)
                {
                    CHECK_QTHREADEX_STOP

                    _deviceHandle = usb_open(device);
                    if (_deviceHandle)
                        break;

                    log_error_m << "Failed open USB interface. Attempt: " << attempts;

                    if (++attempts > 3)
                        break;
                    sleep(3);
                }
                if (threadStop())
                    break;

                if (!_deviceHandle)
                    continue;

                log_info_m << "USB interface is open";

                if (device->num_children != 0 )
                {
                    log_error_m << "Children not supported. USB interface will be closed";
                    USB_CLOSE;
                    continue;
                }

                char buff[128];
                int inum = device->descriptor.iManufacturer;
                if (usb_get_string_simple(_deviceHandle, inum, buff, sizeof(buff)) > 0)
                {
                    log_verbose_m << "USB manufacturer: " << buff;
                }
                inum = device->descriptor.iProduct;
                if (usb_get_string_simple(_deviceHandle, inum, buff, sizeof(buff)) <= 0)
                {
                    log_error_m << "Failed get product name"
                                << ". Detail: " << usb_strerror()
                                << ". USB interface will be closed";
                    USB_CLOSE;
                    continue;
                }

                QString product = QString::fromLatin1(buff);
                log_verbose_m << "USB product: " << product;

                int len = strlen(baseProductName);
                if (strncmp(buff, baseProductName, len) != 0)
                {
                    log_error_m << log_format(
                        "The base name of product must be %?"
                        ". USB interface will be closed", baseProductName);
                    USB_CLOSE;
                    continue;
                }
                if (strlen(buff) != size_t(len + 1))
                {
                    log_error_m << log_format(
                        "The base product name does not contain a product index"
                        ". USB interface will be closed", baseProductName);
                    USB_CLOSE;
                    continue;
                }

                int relayCount = int(buff[len]) - int('0');
                QSet<int> countCheck {1, 2, 4, 8};
                if (!countCheck.contains(relayCount))
                {
                    log_error_m << log_format(
                        "The number of relays must be one of values  [1, 2, 4, 8]"
                        "Current value %?. USB interface will be closed", relayCount);
                    USB_CLOSE;
                    continue;
                }
                log_verbose_m << "USB relay count: " << relayCount;

                int states = readStates(buff, 8);
                if (states < 0)
                {
                    USB_CLOSE;
                    continue;
                }

                const int serialLen = 5;
                for (int i = 0; i < serialLen; ++i)
                {
                    uchar ch = uchar(buff[i]);
                    if ((ch <= 0x20) || (ch >= 0x7F))
                    {
                        log_error_m << log_format(
                            "Incorrect USB relay serial. Symbol index: %?; code: %?",
                            i, int(ch));
                    }
                }
                if (buff[serialLen + 1] != 0)
                {
                    log_error_m << "Bad USB relay serial string";
                    USB_CLOSE;
                    continue;
                }

                QString serial = QString::fromLatin1(buff);
                log_verbose_m << "USB relay serial: " << serial;

                { //Block for QMutexLocker
                    QMutexLocker locker {&_threadLock}; (void) locker;
                    _product = product;
                    _serial = serial;
                    _states = quint8(states);
                    _count = relayCount;

                    QVariant vstat;
                    vstat.setValue(statesInternal());
                    log_verbose_m << "USB relay states: " << vstat;
                }
                return true;
            }
        }
    }

    #undef USB_CLOSE

    log_debug2_m << "Device not found";
    _deviceHandle = nullptr;
    return false;
}

void Relay::releaseDevice()
{
    if (_deviceHandle)
    {
        usb_close(_deviceHandle);
        log_verbose_m << "USB interface closed";

        _deviceHandle = nullptr;
        _deviceInitialized = false;
        _usbContinuousErrors = 0;
        _usbLastErrorCode = 0;
        _product.clear();
        _serial.clear();
        _count = 0;
    }
}

int Relay::readStates(char* buff, int buffSize)
{
    int res = usb_control_msg(_deviceHandle,
                              USB_TYPE_CLASS | USB_RECIP_DEVICE | USB_ENDPOINT_IN,
                              USBRQ_HID_GET_REPORT,
                              0, // value
                              0, // index
                              buff, buffSize /*8*/,
                              REPORT_REQUEST_TIMEOUT);
    if (res != buffSize)
    {
        alog::Line logLine =
            log_error_m << "Failed send message to USB interface";
        if (res < 0)
        {
            _usbLastErrorCode = res;
            logLine << ". Error code: " << res << "; " << usb_strerror();
        }
        ++_usbContinuousErrors;
        return -1;
    }

    if (alog::logger().level() >= alog::Level::Debug)
    {
        if (_usbContinuousErrors != 0)
            log_debug_m << "USB continuous errors: " << int(_usbContinuousErrors);
    }
    _usbContinuousErrors = 0;
    _usbLastErrorCode = 0;
    return quint8(buff[7]); // Байт 7 содержит битовые флаги состояний реле
}

QVector<int> Relay::states() const
{
    QMutexLocker locker {&_threadLock}; (void) locker;
    return statesInternal();
}

QVector<int> Relay::statesInternal() const
{
    QVector<int> st;
    st.resize(_count);
    for (int i = 0; i < _count; ++i)
        st[i] = bool(_states & (1U << i));

    return st;
}

//bool Relay::toggle(const QVector<int> states)
//{
//}

bool Relay::toggle(int relayNumber, bool value)
{
    QMutexLocker locker {&_threadLock}; (void) locker;
    return toggleInternal(relayNumber, value);
}

bool Relay::toggleInternal(int relayNumber, bool value)
{
    if (!_deviceInitialized)
    {
        alog::Line logLine =
            log_error_m << "Failed toggle relay. Device not initialized";
        emit failChange(relayNumber, QString::fromStdString(logLine.impl->buff));
        return false;
    }

    int relayCount = _count;
    if (relayNumber > relayCount)
    {
        alog::Line logLine = log_error_m << log_format(
            "Failed toggle relay number %?. Number out of range [1..%?]",
            relayNumber, count());

        emit failChange(relayNumber, QString::fromStdString(logLine.impl->buff));
        return false;
    }

    int  res;
    char buff[8];
    int  buffSize = sizeof(buff);

    quint8 cmd1 = 0;
    quint8 cmd2 = 0;
    quint8 expectState = 0;

    if (relayNumber <= 0)
    {
        if (value == true)
        {
            cmd1 = 0xFE; // Включить все реле
            expectState = (1U << relayCount) - 1;
        }
        else
        {
            cmd1 = 0xFC; // Выключить все реле
            expectState = 0;
        }
    }
    else
    {
        memset(buff, 0, buffSize);
        res = readStates(buff, buffSize);
        if (res < 0)
        {
            alog::Line logLine = log_error_m << "Failed get relays current state";
            emit failChange(relayNumber, QString::fromStdString(logLine.impl->buff));
            return false;
        }

        expectState = quint8(res);

        if (value == true)
        {
            cmd1 = 0xFF; // Включить реле по номеру
            expectState |= (1U << (relayNumber - 1));
        }
        else
        {
            cmd1 = 0xFD; // Выключить реле по номеру
            expectState &= ~(1U << (relayNumber - 1));
        }
        cmd2 = quint8(relayNumber);
    }

    memset(buff, 0, sizeof(buff));
    buff[0] = cmd1;
    buff[1] = cmd2;
    res = usb_control_msg(_deviceHandle,
                          USB_TYPE_CLASS | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
                          USBRQ_HID_SET_REPORT,
                          //USB_HID_REPORT_TYPE_FEATURE << 8 | (reportId & 0xff), // value
                          0, // value
                          0, // index
                          buff, buffSize,
                          REPORT_REQUEST_TIMEOUT);
    if (res != buffSize)
    {
        alog::Line logLine =
            log_error_m << "Failed send message to USB interface";
        if (res < 0)
        {
            _usbLastErrorCode = res;
            logLine << ". Error code: " << res << "; " << usb_strerror();
        }
        ++_usbContinuousErrors;
        emit failChange(relayNumber, QString::fromStdString(logLine.impl->buff));
        return false;
    }

    res = readStates(buff, buffSize);
    if (res < 0)
    {
        alog::Line logLine = log_error_m << "Failed get relays current state";
        emit failChange(relayNumber, QString::fromStdString(logLine.impl->buff));
        return false;
    }
    _states = quint8(res);

    if (_states != expectState)
    {
        alog::Line logLine = log_error_m << "Failed set relays to new state";
        emit failChange(relayNumber, QString::fromStdString(logLine.impl->buff));
        return false;
    }

    if (relayNumber <= 0)
        log_verbose_m << log_format(
            "USB all relay turn %?", (value) ? "ON" : "OFF");
    else
        log_verbose_m << log_format(
            "USB relay %? turn %?", relayNumber, (value) ? "ON" : "OFF");

    emit changed(relayNumber);

    _usbContinuousErrors = 0;
    _usbLastErrorCode = 0;
    return true;
}

Relay& relay()
{
    return ::safe_singleton<Relay>();
}

} // namespace usb

#undef log_error_m
#undef log_warn_m
#undef log_info_m
#undef log_verbose_m
#undef log_debug_m
#undef log_debug2_m
