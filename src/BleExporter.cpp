#include "BleExporter.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <algorithm>

// Nordic UART Service -- a widely used BLE quasi-standard that many generic
// BLE terminal apps (e.g. "Serial Bluetooth Terminal") already support
// without any dedicated app development.
static const char* NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_CHAR_TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // notify, device->phone
static const char* NUS_CHAR_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // write, unused,
                                                                               // only for app compatibility

static const uint16_t BLE_PREFERRED_MTU       = 247;  // requested; unnegotiated it's only 23
static const size_t   BLE_CHUNK_FALLBACK      = 20;   // 23 - 3 byte ATT header, always safe
static const size_t   BLE_CHUNK_MAX           = 200;  // cap, in case the peer reports a very large MTU
static const uint32_t BLE_CHUNK_DELAY_MS      = 10;   // otherwise congestion in the BLE stack (see BLE_uart example)
static const uint32_t BLE_RESUME_ADV_DELAY_MS = 500;  // give the stack time to settle after a disconnect

class BleServerCallbacksImpl : public BLEServerCallbacks {
public:
  explicit BleServerCallbacksImpl(BleExporter& owner) : owner_(owner) {}

  void onConnect(BLEServer*) override {
    owner_.connected_ = true;
    owner_.disconnectPending_ = false;
    owner_.connectionChanged_ = true;
  }

  void onDisconnect(BLEServer*) override {
    owner_.connected_ = false;
    owner_.disconnectPending_ = true;
    owner_.disconnectAtMs_ = millis();
    owner_.connectionChanged_ = true;
  }

private:
  BleExporter& owner_;
};

// RX channel is not evaluated (we don't need any input from the phone), but
// must exist so that generic BLE terminal apps recognize the device as
// "UART-compatible".
class NoopRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic*) override {}
};

void BleExporter::begin(const char* deviceName) {
  if (advertising_) return;

  if (!initialized_) {
    BLEDevice::init(deviceName);
    BLEDevice::setMTU(BLE_PREFERRED_MTU);
    // Thermals + deliberately short range (~2-5m): the default would be +3dBm
    // (ESP_PWR_LVL_P3, see esp_bt.h). -9dBm is a starting value -- may need
    // adjustment on real hardware, the actual range depends heavily on the
    // antenna/environment and can't be reliably predicted in advance.
    BLEDevice::setPower(ESP_PWR_LVL_N9);

    server_ = BLEDevice::createServer();
    server_->setCallbacks(new BleServerCallbacksImpl(*this));

    BLEService* service = server_->createService(NUS_SERVICE_UUID);

    txChar_ = service->createCharacteristic(NUS_CHAR_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    txChar_->addDescriptor(new BLE2902());

    BLECharacteristic* rxChar = service->createCharacteristic(NUS_CHAR_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
    rxChar->setCallbacks(new NoopRxCallbacks());

    service->start();

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE_UUID);
    adv->setScanResponse(true);

    initialized_ = true;
  }

  connected_ = false;
  disconnectPending_ = false;
  BLEDevice::startAdvertising();
  advertising_ = true;
}

void BleExporter::end() {
  if (!advertising_) return;

  // Feels to the user like "Bluetooth off": an existing connection is
  // actively disconnected instead of merely becoming invisible.
  if (connected_ && server_) {
    server_->disconnect(server_->getConnId());
  }
  BLEDevice::stopAdvertising();

  advertising_ = false;
  connected_ = false;
  disconnectPending_ = false;
}

void BleExporter::loop() {
  if (!advertising_ || !disconnectPending_) return;
  if (millis() - disconnectAtMs_ >= BLE_RESUME_ADV_DELAY_MS) {
    server_->startAdvertising();
    disconnectPending_ = false;
  }
}

bool BleExporter::takeConnectionChanged() {
  if (!connectionChanged_) return false;
  connectionChanged_ = false;
  return true;
}

bool BleExporter::send(const std::string& payload, ProgressCallback onProgress) {
  if (!advertising_ || !connected_ || !txChar_) return false;

  size_t mtu = server_->getPeerMTU(server_->getConnId());
  size_t chunkSize = (mtu > 3) ? (mtu - 3) : BLE_CHUNK_FALLBACK;
  chunkSize = std::max(chunkSize, BLE_CHUNK_FALLBACK);
  chunkSize = std::min(chunkSize, BLE_CHUNK_MAX);

  size_t total = payload.size();
  size_t sent = 0;
  while (sent < total) {
    if (!connected_) return false;  // disconnected while sending
    size_t n = std::min(chunkSize, total - sent);
    txChar_->setValue(payload.substr(sent, n));
    txChar_->notify();
    sent += n;
    delay(BLE_CHUNK_DELAY_MS);
    if (onProgress) onProgress(sent, total);
  }
  return true;
}
