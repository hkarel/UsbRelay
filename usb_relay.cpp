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
    int res = libusb_init(&_context);
    if (res != LIBUSB_SUCCESS)
    {
        log_error_m << "Failed libusb init"
                    << ". Error code: " << res
                    << ". Detail: " << libusb_error_name(res);
        return false;
    }
    return true;
}

void Relay::deinit()
{
    libusb_exit(_context);
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

    int res = libusb_control_transfer(_deviceHandle,
                                LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_ENDPOINT_OUT,
                                USBRQ_HID_SET_REPORT,
                                0, // value
                                0, // index
                                (uchar*)buff, buffSize,
                                REPORT_REQUEST_TIMEOUT);
    if (res != buffSize)
    {
        alog::Line logLine =
            log_error_m << "Failed set USB relay serial: " << val;
        if (res < 0)
        {
            _usbLastErrorCode = res;
            logLine << ". Error code: " << res
                    << ". Detail: " << libusb_error_name(res);
        }
        ++_usbContinuousErrors;
        return false;
    }

    memset(buff, 0, buffSize);
    if (readStates(buff, buffSize) < 0)
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

QString Relay::attachSerial() const
{
    QMutexLocker locker {&_threadLock}; (void) locker;
    return _attachSerial;
}

void Relay::setAttachSerial(const QString& val)
{
    QMutexLocker locker {&_threadLock}; (void) locker;
    _attachSerial = val;
}

int Relay::count() const
{
    QMutexLocker locker {&_threadLock}; (void) locker;
    return _count;
}

bool Relay::claimDevice()
{
    int res;
    bool deviceFound = false;

    libusb_device** devList;
    ssize_t devCount = libusb_get_device_list(0, &devList);

    #define USB_DEV_CLOSE { \
        libusb_close(_deviceHandle); \
        _deviceHandle = nullptr; \
        log_verbose_m << "USB device closed"; }

    for (ssize_t i = 0; i < devCount; ++i)
    {
        CHECK_QTHREADEX_STOP

        libusb_device* device = devList[i];
        libusb_device_descriptor descript;
        res = libusb_get_device_descriptor(device, &descript);
        if (res != LIBUSB_SUCCESS)
        {
            log_error_m << "Failed get device descriptor"
                        << ". Error code: " << res
                        << ". Detail: " << libusb_error_name(res);
            continue;
        }

        if (descript.idVendor == USB_RELAY_VENDOR_ID
            && descript.idProduct == USB_RELAY_DEVICE_ID)
        {
            deviceFound = true;
            _usbBusNumber = libusb_get_bus_number(device);
            _usbDeviceNumber = libusb_get_device_address(device);

            log_info_m << "USB device found on bus "
                       << utl::formatMessage("%03d/%03d", _usbBusNumber, _usbDeviceNumber);

            res = libusb_open(device, &_deviceHandle);
            if (res != LIBUSB_SUCCESS)
            {
                log_error_m << "Failed open USB device"
                            << ". Error code: " << res
                            << ". Detail: " << libusb_error_name(res);

                _deviceHandle = nullptr;
                continue;
            }
            log_verbose_m << "USB device is open";

            /** TODO Аналог num_children в linusb-1.0 не найден **
            if (device->num_children != 0)
            {
                log_error_m << "Children not supported. USB interface will be closed";
                USB_CLOSE;
                continue;
            } */

            char buff[128];
            res = libusb_get_string_descriptor_ascii(
                                        _deviceHandle, descript.iManufacturer,
                                        (uchar*)buff, sizeof(buff));
            if (res < LIBUSB_SUCCESS)
            {
                log_error_m << "Failed get manufacturer description"
                            << ". Error code: " << res
                            << ". Detail: " << libusb_error_name(res);
                USB_DEV_CLOSE;
                continue;
            }
            log_verbose_m << "USB manufacturer: " << buff;

            res = libusb_get_string_descriptor_ascii(
                                        _deviceHandle, descript.iProduct,
                                        (uchar*)buff, sizeof(buff));
            if (res < LIBUSB_SUCCESS)
            {
                log_error_m << "Failed get product description"
                            << ". Error code: " << res
                            << ". Detail: " << libusb_error_name(res);
                USB_DEV_CLOSE;
                continue;
            }
            QString product = QString::fromLatin1(buff);
            log_verbose_m << "USB product: " << product;

            int len = strlen(baseProductName);
            if (strncmp(buff, baseProductName, len) != 0)
            {
                log_error_m << log_format(
                    "The base name of product must be %?"
                    ". USB device will be closed", baseProductName);
                USB_DEV_CLOSE;
                continue;
            }
            if (strlen(buff) != size_t(len + 1))
            {
                log_error_m << log_format(
                    "The base product name does not contain a product index"
                    ". USB device will be closed", baseProductName);
                USB_DEV_CLOSE;
                continue;
            }

            int relayCount = int(buff[len]) - int('0');
            QSet<int> countCheck {1, 2, 4, 8};
            if (!countCheck.contains(relayCount))
            {
                log_error_m << log_format(
                    "The number of relays must be one of values  [1, 2, 4, 8]"
                    "Current value %?. USB device will be closed", relayCount);
                USB_DEV_CLOSE;
                continue;
            }
            log_verbose_m << "USB relay count: " << relayCount;

            // Чтение состояний реле и серийного номера
            int states = readStates(buff, 8);
            if (states < 0)
            {
                USB_DEV_CLOSE;
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
                USB_DEV_CLOSE;
                continue;
            }

            QString serial = QString::fromLatin1(buff);
            log_verbose_m << "USB relay serial: " << serial;

            { //Block for QMutexLocker
                QMutexLocker locker {&_threadLock}; (void) locker;
                if (!_attachSerial.isEmpty())
                {
                    if (_attachSerial != serial)
                    {
                        log_verbose_m << log_format(
                            "USB relay serial (%?) not match attach-serial (%?)",
                            serial, _attachSerial);
                        USB_DEV_CLOSE;
                        continue;
                    }
                    log_verbose_m << "USB relay serial to match attach-serial";
                }
            }

            libusb_config_descriptor* config;
            res = libusb_get_active_config_descriptor(device, &config);
            if (res != LIBUSB_SUCCESS)
            {
                log_error_m << "Failed libusb_get_active_config_descriptor"
                            << ". Error code: " << res
                            << ". Detail: " << libusb_error_name(res);
                USB_DEV_CLOSE;
                continue;
            }

            res = libusb_set_auto_detach_kernel_driver(_deviceHandle, 1);
            if (res != LIBUSB_SUCCESS)
            {
                log_error_m << "Failed set auto_detach_kernel_driver flag"
                            << ". Error code: " << res
                            << ". Detail: " << libusb_error_name(res);
                USB_DEV_CLOSE;
                continue;
            }

            const int intfNumber = 0;
            res = libusb_claim_interface(_deviceHandle, intfNumber);
            if (res != LIBUSB_SUCCESS)
            {
                log_error_m << "Failed claim USB interface " << intfNumber
                            << ". Error code: " << res
                            << ". Detail: " << libusb_error_name(res)
                            << ". Perhaps need to create a UDEV rule to access the device";
                USB_DEV_CLOSE;
                continue;
            }
            log_verbose_m << log_format("USB interface %? claimed", intfNumber);

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

            libusb_free_device_list(devList, true);
            return true;
        }
    }

    #undef USB_DEV_CLOSE

    /* Free the libusb device list freeing unused devices */
    libusb_free_device_list(devList, true);

    if (deviceFound)
        log_debug_m << "Device failed initialize";
    else
        log_debug_m << "Device not found";

    _deviceHandle = nullptr;
    return false;
}

void Relay::releaseDevice(bool deviceDetached)
{
    if (_deviceHandle)
    {
        if (!deviceDetached)
        {
            const int intfNumber = 0;
            int res = libusb_release_interface(_deviceHandle, intfNumber);
            if (res != LIBUSB_SUCCESS)
                log_error_m << "Failed release USB interface " << intfNumber
                            << ". Error code: " << res
                            << ". Detail: " << libusb_error_name(res);
            else
                log_verbose_m << log_format("USB interface %? released", intfNumber);
        }
        libusb_close(_deviceHandle);
        log_verbose_m << "USB device closed";
    }
    _deviceHandle = nullptr;
    _deviceInitialized = false;
    _usbContinuousErrors = 0;
    _usbLastErrorCode = 0;
    _product.clear();
    _serial.clear();
    _count = 0;
}

void Relay::run()
{
    log_info_m << "Started";

    quint32 claimAttempts = 0;
    bool deviceDetached = false;

    while (true)
    {
        if (threadStop())
            break;

        _deviceInitialized = false;
        if (!claimDevice())
        {
            releaseDevice(false);
            int timeout = 2;
            if (claimAttempts > 40)
                timeout = 15;
            else if (claimAttempts > 20)
                timeout = 10;

            sleep(timeout);
            claimAttempts++;
            CHECK_QTHREADEX_STOP
            continue;
        }
        claimAttempts = 0;
        deviceDetached = false;
        _deviceInitialized = true;

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

        log_info_m << "USB relay emit signal 'attached'";
        emit attached();

        while (true)
        {
            CHECK_QTHREADEX_STOP

            if (_usbContinuousErrors >= USB_CONTINUOUS_ERRORS_1
                && _usbLastErrorCode == LIBUSB_ERROR_NO_DEVICE)
            {
                deviceDetached = true;
                break;
            }
            if (_usbContinuousErrors >= USB_CONTINUOUS_ERRORS_2)
            {
                deviceDetached = true;
                break;
            }

            { //Block for QMutexLocker
                QMutexLocker locker(&_threadLock); (void) locker;
                _threadCond.wait(&_threadLock, 200);
            }
            if (threadStop())
                continue;

            { //Block for QMutexLocker
                QMutexLocker locker {&_threadLock}; (void) locker;
                char buff[8] = {0};
                int states = readStates(buff, sizeof(buff));
                if (states < 0)
                    continue;

                if (_states != quint8(states))
                {
                    log_debug_m << log_format(
                        "USB relay state was changed from outside"
                        ". Old value: %?. New value: %?", int(_states), states);
                    _states = quint8(states);
                }
            }
        } // while (true)

        log_info_m << "USB relay emit signal 'detached'";
        emit detached();

        releaseDevice(deviceDetached);

    } // while (true)

    releaseDevice(deviceDetached);

    log_info_m << "Stopped";
}

void Relay::threadStopEstablished()
{
    QMutexLocker locker {&_threadLock}; (void) locker;
    _threadCond.wakeAll();
}

int Relay::readStates(char* buff, int buffSize)
{
    int res = libusb_control_transfer(_deviceHandle,
                                LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_ENDPOINT_IN,
                                USBRQ_HID_GET_REPORT,
                                0, // value
                                0, // index
                                (uchar*)buff, buffSize,
                                REPORT_REQUEST_TIMEOUT);
    if (res != buffSize)
    {
        alog::Line logLine =
            log_error_m << "Failed send message to USB interface";
        if (res < 0)
        {
            _usbLastErrorCode = res;
            logLine << ". Error code: " << res
                    << ". Detail: " << libusb_error_name(res);
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

    char buff[8];
    int  buffSize = sizeof(buff);

    quint8 cmd1 = 0;
    quint8 cmd2 = 0;
    quint8 expectStates = 0;

    if (relayNumber <= 0)
    {
        if (value == true)
        {
            cmd1 = 0xFE; // Включить все реле
            expectStates = (1U << relayCount) - 1;
        }
        else
        {
            cmd1 = 0xFC; // Выключить все реле
            expectStates = 0;
        }
        relayNumber = 0;
    }
    else
    {
        memset(buff, 0, buffSize);
        int states = readStates(buff, buffSize);
        if (states < 0)
        {
            alog::Line logLine = log_error_m << "Failed get relays current state";
            emit failChange(relayNumber, QString::fromStdString(logLine.impl->buff));
            return false;
        }
        expectStates = quint8(states);

        if (value == true)
        {
            cmd1 = 0xFF; // Включить реле по номеру
            expectStates |= (1U << (relayNumber - 1));
        }
        else
        {
            cmd1 = 0xFD; // Выключить реле по номеру
            expectStates &= ~(1U << (relayNumber - 1));
        }
        cmd2 = quint8(relayNumber);
    }

    memset(buff, 0, sizeof(buff));
    buff[0] = cmd1;
    buff[1] = cmd2;
    int res = libusb_control_transfer(_deviceHandle,
                                LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_ENDPOINT_OUT,
                                USBRQ_HID_SET_REPORT,
                                0, // value
                                0, // index
                                (uchar*)buff, buffSize,
                                REPORT_REQUEST_TIMEOUT);
    if (res != buffSize)
    {
        alog::Line logLine =
            log_error_m << "Failed send message to USB interface";
        if (res < 0)
        {
            _usbLastErrorCode = res;
            logLine << ". Error code: " << res
                    << ". Detail: " << libusb_error_name(res);
        }
        ++_usbContinuousErrors;
        emit failChange(relayNumber, QString::fromStdString(logLine.impl->buff));
        return false;
    }

    int states = readStates(buff, buffSize);
    if (states < 0)
    {
        alog::Line logLine = log_error_m << "Failed get relays current state";
        emit failChange(relayNumber, QString::fromStdString(logLine.impl->buff));
        return false;
    }
    _states = quint8(states);

    if (_states != expectStates)
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
