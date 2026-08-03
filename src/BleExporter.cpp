#include "BleExporter.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <algorithm>

// Nordic UART Service -- weit verbreiteter BLE-Quasi-Standard, den viele
// generische BLE-Terminal-Apps (z.B. "Serial Bluetooth Terminal") bereits
// ohne eigene App-Entwicklung unterstuetzen.
static const char* NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_CHAR_TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // notify, Geraet->Handy
static const char* NUS_CHAR_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // write, ungenutzt,
                                                                               // nur fuer App-Kompatibilitaet

static const uint16_t BLE_PREFERRED_MTU       = 247;  // angefragt; unverhandelt sind es nur 23
static const size_t   BLE_CHUNK_FALLBACK      = 20;   // 23 - 3 Byte ATT-Header, immer sicher
static const size_t   BLE_CHUNK_MAX           = 200;  // Deckel, falls Peer ein sehr grosses MTU meldet
static const uint32_t BLE_CHUNK_DELAY_MS      = 10;   // sonst Stau im BLE-Stack (siehe BLE_uart-Beispiel)
static const uint32_t BLE_RESUME_ADV_DELAY_MS = 500;  // Stack Zeit geben, sich nach Trennung zu beruhigen

class BleServerCallbacksImpl : public BLEServerCallbacks {
public:
  explicit BleServerCallbacksImpl(BleExporter& owner) : owner_(owner) {}

  void onConnect(BLEServer*) override {
    owner_.connected_ = true;
    owner_.disconnectPending_ = false;
  }

  void onDisconnect(BLEServer*) override {
    owner_.connected_ = false;
    owner_.disconnectPending_ = true;
    owner_.disconnectAtMs_ = millis();
  }

private:
  BleExporter& owner_;
};

// RX-Kanal wird nicht ausgewertet (wir brauchen keine Eingabe vom Handy),
// muss aber existieren, damit generische BLE-Terminal-Apps das Geraet als
// "UART-kompatibel" erkennen.
class NoopRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic*) override {}
};

void BleExporter::begin(const char* deviceName) {
  if (advertising_) return;

  if (!initialized_) {
    BLEDevice::init(deviceName);
    BLEDevice::setMTU(BLE_PREFERRED_MTU);

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

  // Fuehlt sich fuer den Nutzer wie "Bluetooth aus" an: eine bestehende
  // Verbindung wird aktiv getrennt statt nur unsichtbar zu werden.
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

bool BleExporter::send(const std::string& payload, ProgressCallback onProgress) {
  if (!advertising_ || !connected_ || !txChar_) return false;

  size_t mtu = server_->getPeerMTU(server_->getConnId());
  size_t chunkSize = (mtu > 3) ? (mtu - 3) : BLE_CHUNK_FALLBACK;
  chunkSize = std::max(chunkSize, BLE_CHUNK_FALLBACK);
  chunkSize = std::min(chunkSize, BLE_CHUNK_MAX);

  size_t total = payload.size();
  size_t sent = 0;
  while (sent < total) {
    if (!connected_) return false;  // waehrend des Sendens getrennt
    size_t n = std::min(chunkSize, total - sent);
    txChar_->setValue(payload.substr(sent, n));
    txChar_->notify();
    sent += n;
    delay(BLE_CHUNK_DELAY_MS);
    if (onProgress) onProgress(sent, total);
  }
  return true;
}
