#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

class BLEServer;
class BLECharacteristic;

// Nordic-UART-Service-BLE-Export. Vorwaertsdeklarationen oben halten die
// vollen BLE-Header aus main.cpp heraus (nur BleExporter.cpp braucht sie) --
// analog dazu, wie DisplayViews.h nie AS7341Spectrometer.h mitzieht.
class BleExporter {
public:
  using ProgressCallback = void (*)(size_t sentBytes, size_t totalBytes);

  void begin(const char* deviceName);  // idempotent: no-op falls schon aktiv
  void end();                          // idempotent: no-op falls nicht aktiv
  bool isActive() const { return advertising_; }  // begin() wurde aufgerufen, end() noch nicht
  bool isConnected() const { return connected_; }

  // Liefert true GENAU EINMAL nach einem Verbindungswechsel (connect ODER
  // disconnect) seit dem letzten Aufruf, danach false bis zum naechsten
  // Wechsel -- Pull statt Push (main.cpp wird nie direkt aus dem Bluedroid-
  // Callback-Kontext heraus aufgerufen), analog zu isConnected()/isActive().
  // main.cpp nutzt das, um den Export-Screen GENAU DANN neu zu zeichnen, wenn
  // sich der (asynchron im BLE-Stack geaenderte) Verbindungsstatus tatsaechlich
  // geaendert hat, statt ihn blind periodisch zu pollen.
  bool takeConnectionChanged();

  // Sendet 'payload' vollstaendig, in an das ausgehandelte MTU angepassten
  // Haeppchen mit kurzer Pause dazwischen (sonst Stau im BLE-Stack). Gibt
  // sofort false zurueck, falls nicht verbunden; bricht ab (false), falls
  // die Verbindung waehrend des Sendens verloren geht.
  bool send(const std::string& payload, ProgressCallback onProgress = nullptr);

  // Nicht-blockierende Haushaltsarbeit (Wiederaufnahme der Werbung nach
  // Verbindungsabbruch); einmal pro loop()-Durchlauf aufrufen, billiges
  // No-op wenn nicht aktiv.
  void loop();

private:
  BLEServer* server_ = nullptr;
  BLECharacteristic* txChar_ = nullptr;
  // initialized_ wird NUR beim allerersten begin() gesetzt und nie wieder
  // zurueckgesetzt: BLEDevice::init()/deinit() wiederholt aufzurufen ist ein
  // bekanntes Problem im ESP32-Bluedroid-Stack und fuehrt reproduzierbar zum
  // Haengenbleiben beim zweiten init(). end() ruft deshalb bewusst nie
  // deinit() auf, sondern stoppt nur die Werbung + trennt eine bestehende
  // Verbindung -- der Stack bleibt danach im Hintergrund resident.
  bool initialized_ = false;
  bool advertising_ = false;
  volatile bool connected_ = false;
  volatile bool connectionChanged_ = false;
  bool disconnectPending_ = false;
  uint32_t disconnectAtMs_ = 0;

  friend class BleServerCallbacksImpl;
};
