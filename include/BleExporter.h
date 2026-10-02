#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

class BLEServer;
class BLECharacteristic;

// Nordic UART Service BLE export. The forward declarations above keep the
// full BLE headers out of main.cpp (only BleExporter.cpp needs them) --
// analogous to how DisplayViews.h never pulls in AS7341Spectrometer.h.
class BleExporter {
public:
  using ProgressCallback = void (*)(size_t sentBytes, size_t totalBytes);

  void begin(const char* deviceName);  // idempotent: no-op if already active
  void end();                          // idempotent: no-op if not active
  bool isActive() const { return advertising_; }  // begin() was called, end() not yet
  bool isConnected() const { return connected_; }

  // Returns true EXACTLY ONCE after a connection change (connect OR
  // disconnect) since the last call, then false until the next change --
  // pull instead of push (main.cpp is never called directly from the
  // Bluedroid callback context), analogous to isConnected()/isActive().
  // main.cpp uses this to redraw the export screen EXACTLY WHEN the
  // (asynchronously changed, within the BLE stack) connection status has
  // actually changed, instead of blindly polling it periodically.
  bool takeConnectionChanged();

  // Sends 'payload' completely, in chunks sized to the negotiated MTU with a
  // short pause in between (otherwise congestion in the BLE stack). Returns
  // false immediately if not connected; aborts (false) if the connection is
  // lost while sending.
  bool send(const std::string& payload, ProgressCallback onProgress = nullptr);

  // Non-blocking housekeeping (resuming advertising after a disconnect);
  // call once per loop() iteration, cheap no-op when not active.
  void loop();

private:
  BLEServer* server_ = nullptr;
  BLECharacteristic* txChar_ = nullptr;
  // initialized_ is set ONLY on the very first begin() and never reset
  // again: repeatedly calling BLEDevice::init()/deinit() is a known problem
  // in the ESP32 Bluedroid stack and reproducibly causes it to hang on the
  // second init(). end() therefore deliberately never calls deinit(), but
  // only stops advertising + disconnects an existing connection -- the
  // stack remains resident in the background afterward.
  bool initialized_ = false;
  bool advertising_ = false;
  volatile bool connected_ = false;
  volatile bool connectionChanged_ = false;
  bool disconnectPending_ = false;
  uint32_t disconnectAtMs_ = 0;

  friend class BleServerCallbacksImpl;
};
