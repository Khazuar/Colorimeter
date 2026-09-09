# Reflectance Colorimeter — Firmware

Firmware for a DIY 45°/0° reflectance colorimeter. It reads 10 spectral
channels from an AS7341 sensor, drives a Nichia Optisolis reference LED,
shows results on an SSD1306 OLED display, and can transfer measurements
over BLE. Runs on an ESP32-C3.

The device measures the reflectance of a sample (dyed filament, wall
paint, textiles, ...) against a white reference and computes CIE L\*a\*b\*
color values from it.

## Hardware

This firmware is written for a specific 3D-printed device. Print files,
bill of materials, wiring diagram, and full assembly instructions are
here:

👉 **https://www.printables.com/model/1833812-reflectance-colorimeter**

You need the assembled device — or at least the wired electronics
(ESP32-C3, AS7341, SSD1306, two buttons, LED) — before this firmware is
useful to you.

## Status

This is a prototype under active development. Expect rough edges and
incomplete documentation in places. Bug reports, questions, and PRs are
welcome via Issues.

## Building

Requires [PlatformIO](https://platformio.org/) (VS Code extension or
CLI).

```bash
git clone <repo-url>
cd <repo-folder>
pio run
```

This compiles the firmware without flashing it.

## Flashing

The ESP32-C3 needs to be put into bootloader/download mode before you
can flash it. On this device, the **Mode** button doubles as the
flashing button — it's wired in parallel to the ESP32-C3's native BOOT
strapping pin (GPIO9), so you don't need to open the case or reach a
hidden button with a needle.

1. Unplug the device.
2. **Hold down the Mode button.**
3. While still holding it, plug in a USB-C cable that supports data
   transfer (many charging-only cables won't work).
4. Release the button once connected. Your computer should now enumerate
   a different USB device than during normal operation (bootloader mode
   instead of the regular USB-JTAG serial device).
5. Flash:

   ```bash
   pio run --target upload
   ```

   PlatformIO should auto-detect the port. If it picks the wrong one,
   specify it manually, e.g.:

   ```bash
   pio run --target upload --upload-port /dev/ttyACM0
   ```

6. Once flashing finishes, unplug and replug the device normally
   (**without** holding Mode) to boot into the newly flashed firmware.

## Monitoring / debugging

```bash
pio device monitor
```

Shows serial/debug output — useful for checking sensor readings and
catching errors during development.
