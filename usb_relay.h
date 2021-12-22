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

#pragma once

#include "shared/defmac.h"
#include "shared/safe_singleton.h"
#include "shared/qt/qthreadex.h"

#include <QtCore>
#include <atomic>
#include <libusb-1.0/libusb.h>

namespace usb {

class Relay : public QThreadEx
{
public:
    bool init(const QVector<int>& states = {});
    void deinit();

    // Наименование продукта
    QString product() const;

    // Строковый идентификатор
    QString serial() const;

    // Устанавливает строковый идентификатор. Длина идентификатора  не  должна
    // превышать 5 символов. Допускается использовать цифры и буквы латинского
    // алфавита
    bool setSerial(const QString& value);

    // Устанавливает ограничение на подключение устройства по серийному номеру.
    // Если параметр не пуст,  то  устройство  будет  подключено  только в том
    // случае, когда его серийный номер совпадает с заданным
    QString attachSerial() const;
    void setAttachSerial(const QString&);

    // Вектор текущих состояний реле
    QVector<int> states() const;

    // Возвращает количество реле в подключенном устройстве
    int count() const;

    // Возвращает TRUE если устройство подключено
    bool isAttached() const {return _deviceInitialized;}

signals:
    // Эмитируется при подключении реле к USB-порту
    void attached();

    // Эмитируется при отключении реле от USB-порта
    void detached();

    // Эмитируется при изменении состояния реле
    void changed(int relayNumber);

    // Эмитируется если не удалось изменить состояние реле
    void failChange(int relayNumber, const QString& errorMessage);

public slots:
    //bool toggle(const QVector<int> states);

    // Активирует/деактивирует реле с номером relayNumber. Нумерация реле
    // начинается с единицы.  Если relayNumber > RelayCount  переключение
    // выполнено не будет. Если relayNumber <= 0  будут  переключены  все
    // реле на плате
    bool toggle(int relayNumber, bool value);

private:
    Q_OBJECT
    Relay() = default;
    DISABLE_DEFAULT_COPY(Relay)

    bool claimDevice();
    void releaseDevice(bool deviceDetached);

    void run() override;
    void threadStopEstablished() override;

    int readStates(char* buff, int buffSize);

    QVector<int> statesInternal() const;
    bool toggleInternal(int relayNumber, bool value);

private:
    int _usbBusNumber = {0};
    int _usbDeviceNumber = {0};

    QVector<int> _initStates;
    QString _attachSerial;

    libusb_context*       _context = {nullptr};
    libusb_device_handle* _deviceHandle = {nullptr};
    std::atomic_bool      _deviceInitialized = {false};
    std::atomic_int       _usbContinuousErrors = {0};
    std::atomic_int       _usbLastErrorCode = {0};

    QString _product;
    QString _serial;
    quint8  _states = {0};
    qint32  _count = {0};

    mutable QMutex _threadLock;
    mutable QWaitCondition _threadCond;

    template<typename T, int> friend T& ::safe_singleton();
};

Relay& relay();

} // namespace usb
